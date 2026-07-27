/**
 * @file wifi_app.cpp
 * @brief WiFi STA + NTP 时间同步
 */

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include "wifi_app.h"

static const char *TAG = "NET";

/* NTP 服务器 */
#define NTP_SERVER1 "pool.ntp.org"
#define NTP_SERVER2 "ntp.aliyun.com"
#define NTP_SERVER3 "time.windows.com"

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static int s_retry_count = 0;
/* STA 自动重连开关: 进 AP 配网前置 false, 避免重连与 WiFi 扫描抢射频 */
static volatile bool s_reconnect_enabled = true;
/* 夜间省电: 射频已 stop, 重连检查跳过 (白天 resume 后清零) */
static volatile bool s_night_sleep = false;
/* 监控屏用: 连接后保存本机 IP / SSID (供 getter 读取) */
static char s_ip_str[16]  = "0.0.0.0";
static char s_ssid_str[33] = "";

/* ===== WiFi 事件处理 ===== */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_reconnect_enabled) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 打印断连原因码, 用于区分网络问题(找不到AP)还是配置问题(密码错) */
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected, reason=%d", d ? d->reason : -1);
        if (!s_reconnect_enabled) {
            /* 配网模式: 不再重连, 让射频空出来给扫描 */
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        } else if (s_retry_count < 5) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "WiFi disconnect, retry %d", s_retry_count);
        } else {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after 5 retries");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ===== 初始化 WiFi STA ===== */
void wifi_init_sta(const char *ssid, const char *password)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    strlcpy(s_ssid_str, ssid, sizeof(s_ssid_str));   /* 监控屏用 */
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    /* 有密码要求至少 WPA2; 空密码则设 OPEN, 否则连开放热点会被阈值拒绝 */
    wifi_config.sta.threshold.authmode =
        (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
}

esp_err_t wifi_wait_connected(int timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

/* 是否处于夜间省电态 (射频已关) — 供 UI 区分"故障断网"与"主动省电" */
bool wifi_is_night_sleep(void)
{
    return s_night_sleep;
}

/* ===== 监控屏 getter: 本机 IP / SSID / RSSI 信号 / 信道 ===== */
const char *wifi_get_ip(void)
{
    return s_ip_str;
}

const char *wifi_get_ssid(void)
{
    return s_ssid_str;
}

/* RSSI (dBm, 负值). 未连接返回 0. */
int wifi_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

/* 当前信道. 未连接返回 0. */
int wifi_get_channel(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.primary;
    return 0;
}

void wifi_check_reconnect(void)
{
    if (!s_reconnect_enabled) return;  /* 配网模式下不重连 */
    if (s_night_sleep) return;         /* 夜间省电: 射频已停, 不重连 */
    if (!wifi_is_connected()) {
        static uint32_t last_try = 0;
        uint32_t now = xTaskGetTickCount() / pdMS_TO_TICKS(1000);
        if (now - last_try > 120) {  /* 每2分钟尝试重连一次 */
            last_try = now;
            s_retry_count = 0;
            ESP_LOGW(TAG, "WiFi auto reconnect...");
            esp_wifi_connect();
        }
    }
}

/* 停止 STA 自动重连并断开当前连接 (进 AP 配网前调用, 释放射频给扫描) */
void wifi_stop_sta_reconnect(void)
{
    s_reconnect_enabled = false;
    s_retry_count = 99;  /* 防止事件处理里再触发重连 */
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "STA reconnect disabled (for provisioning scan)");
}

/* 切换到另一个网络 (WiFi 驱动已初始化后调用, 不重建驱动/事件循环)
 * 用于开机先试主网络失败后, 不解初始化直接切到备用网络再试 */
void wifi_switch_network(const char *ssid, const char *password)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid,     ssid,     sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode =
        (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;
    strlcpy(s_ssid_str, ssid, sizeof(s_ssid_str));
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    ESP_LOGI(TAG, "Switching to fallback WiFi: %s", ssid);
}

/* 夜间省电: 停 WiFi 射频 (关掉最大耗电源). 不销毁驱动/netif, 只是 esp_wifi_stop,
 * 早上 wifi_radio_on 里 esp_wifi_start 即可恢复, 全程 CPU 不睡, 无唤醒风险. */
esp_err_t wifi_radio_off(void)
{
    if (s_night_sleep) return ESP_OK;      /* 已在夜间态, 幂等 */
    s_night_sleep = true;
    s_retry_count = 99;                    /* 事件处理里不再触发重连 */
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_sntp_stop();                       /* 停 SNTP, 否则夜间无网仍后台发包/刷失败日志 */
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();       /* 关射频 (省电大头) */
    ESP_LOGI(TAG, "Night mode: WiFi radio stopped (power save)");
    return err;
}

/* 白天恢复: 重开 WiFi 射频并重连. 返回 ESP_OK 表示 start 成功 (连接由事件异步完成).
 * 若 start 失败, 保持夜间态标志由调用方决定是否下轮重试, 不会卡死. */
esp_err_t wifi_radio_on(void)
{
    if (!s_night_sleep) return ESP_OK;     /* 本就在白天态 */
    esp_err_t err = esp_wifi_start();      /* 重开射频 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Day resume: esp_wifi_start fail (%s), will retry", esp_err_to_name(err));
        return err;                        /* 保持 s_night_sleep=true, 调用方下轮再试 */
    }
    s_night_sleep = false;
    s_retry_count = 0;
    esp_wifi_connect();                    /* STA_START 事件也会触发, 这里再补一次无害 */
    ESP_LOGI(TAG, "Day mode: WiFi radio resumed");
    return ESP_OK;
}

/* ===== NTP 时间同步 ===== */
esp_err_t ntp_sync_time(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "NTP: WiFi not connected");
        return ESP_FAIL;
    }

    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER1);
    esp_sntp_setservername(1, NTP_SERVER2);
    esp_sntp_setservername(2, NTP_SERVER3);
    esp_sntp_init();

    /* 等待 NTP 同步 */
    time_t now = 0;
    struct tm timeinfo = {0};
    int retry = 0;
    while (timeinfo.tm_year < (2024 - 1900) && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        time(&now);
        localtime_r(&now, &timeinfo);
        retry++;
    }

    if (timeinfo.tm_year >= (2024 - 1900)) {
        ESP_LOGI(TAG, "NTP sync OK: %s", asctime(&timeinfo));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "NTP sync timeout");
    return ESP_FAIL;
}

void get_time_str(char *buf)
{
    time_t now = 0;
    struct tm ti = {0};
    time(&now);
    localtime_r(&now, &ti);
    /* 只到分: 反射屏主界面静止时可降到分钟级刷新, 省电且减少刷屏闪动 */
    snprintf(buf, 24, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

void get_date_str(char *buf)
{
    time_t now = 0;
    struct tm ti = {0};
    time(&now);
    localtime_r(&now, &ti);
    const char *wday[] = {"日","一","二","三","四","五","六"};
    sprintf(buf, "%04d/%02d/%02d%s",
            ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
            wday[ti.tm_wday]);
}

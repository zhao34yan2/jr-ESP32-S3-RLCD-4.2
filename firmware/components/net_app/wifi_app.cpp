/**
 * @file wifi_app.cpp
 * @brief WiFi STA + NTP 时间同步
 */

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_sntp.h>
#include "wifi_app.h"

static const char *TAG = "NET";

/* 射频操作互斥锁: 串行化所有复合 esp_wifi_* 序列 (stop/start/connect/set_config).
 * api_task(夜间省电开关/重连) 与 ui_task(短按重启网络) 会并发进这些序列, 无锁时
 * 两任务同时 stop+start 在部分 IDF 上会 assert/状态错乱. 所有对外射频函数入口取此锁. */
static SemaphoreHandle_t s_radio_mutex = NULL;
#define RADIO_LOCK()    do { if (s_radio_mutex) xSemaphoreTake(s_radio_mutex, portMAX_DELAY); } while (0)
#define RADIO_UNLOCK()  do { if (s_radio_mutex) xSemaphoreGive(s_radio_mutex); } while (0)

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

/* 拷贝 SSID 到 wifi_config 的 uint8_t ssid[32] (无独立 ssid_len 字段).
 * strlcpy(dst,src,32) 只写 31 字符+NUL, 会把恰好 32 字节的合法 SSID (802.11 上限) 末字截断→连不上.
 * 改为最多拷 32 字节; 不足 32 才补 NUL, 恰好 32 字节保留全部字符 (依赖调用方已 {0} 清零). */
static void wifi_copy_ssid(uint8_t dst[32], const char *src)
{
    size_t n = strnlen(src, 32);
    memcpy(dst, src, n);
    if (n < 32) dst[n] = '\0';
}

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
        /* 断连即视为 IP 失效, 清掉旧 IP, 免得监控屏显示过期地址 */
        strlcpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));
        if (!s_reconnect_enabled) {
            /* 配网模式: 不再重连, 让射频空出来给扫描 */
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        } else if (s_retry_count < 5) {
            esp_err_t ce = esp_wifi_connect();
            s_retry_count++;
            if (ce != ESP_OK)
                ESP_LOGW(TAG, "esp_wifi_connect ret %s (retry %d)", esp_err_to_name(ce), s_retry_count);
            else
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
    if (!s_radio_mutex) s_radio_mutex = xSemaphoreCreateMutex();
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
    wifi_copy_ssid(wifi_config.sta.ssid, ssid);      /* 32字节 SSID 不截断 (见 wifi_copy_ssid) */
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

/* 一次 esp_wifi_sta_get_ap_info 同时取回 RSSI + 信道 (监控屏原来分两次调用, 合并省一次).
 * 任一出参可为 NULL; 未连接时输出 0. */
void wifi_get_ap_info(int *rssi, int *channel)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        if (rssi)    *rssi    = ap.rssi;
        if (channel) *channel = ap.primary;
    } else {
        if (rssi)    *rssi    = 0;
        if (channel) *channel = 0;
    }
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
            /* 持射频锁再 connect: 避免与 ui_task 的 wifi_restart_radio (off→on)
             * 交错, 否则可能在 stop 与 start 之间插入 connect 触发驱动状态错乱 */
            if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, portMAX_DELAY) == pdTRUE) {
                if (!s_night_sleep && !wifi_is_connected()) {
                    s_retry_count = 0;
                    ESP_LOGW(TAG, "WiFi auto reconnect...");
                    esp_err_t cerr = esp_wifi_connect();
                    if (cerr != ESP_OK)
                        ESP_LOGW(TAG, "auto reconnect esp_wifi_connect ret %s", esp_err_to_name(cerr));
                }
                xSemaphoreGive(s_radio_mutex);
            }
        }
    }
}

/* 停止 STA 自动重连并断开当前连接 (进 AP 配网前调用, 释放射频给扫描) */
void wifi_stop_sta_reconnect(void)
{
    s_reconnect_enabled = false;
    s_retry_count = 99;  /* 防止事件处理里再触发重连 */
    RADIO_LOCK();
    esp_wifi_disconnect();
    RADIO_UNLOCK();
    ESP_LOGI(TAG, "STA reconnect disabled (for provisioning scan)");
}

/* 切换到另一个网络 (WiFi 驱动已初始化后调用, 不重建驱动/事件循环)
 * 用于开机先试主网络失败后, 不解初始化直接切到备用网络再试 */
void wifi_switch_network(const char *ssid, const char *password)
{
    wifi_config_t wc = {0};
    wifi_copy_ssid(wc.sta.ssid, ssid);   /* 32 字节 SSID 不截断 (见 wifi_copy_ssid) */
    strlcpy((char *)wc.sta.password, password, sizeof(wc.sta.password));
    wc.sta.threshold.authmode =
        (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;
    strlcpy(s_ssid_str, ssid, sizeof(s_ssid_str));
    /* 持射频锁: disconnect/set_config/connect 复合序列, 与夜间省电/短按重连串行化 */
    RADIO_LOCK();
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    RADIO_UNLOCK();
    ESP_LOGI(TAG, "Switching to fallback WiFi: %s", ssid);
}

/* ── 射频操作互斥: 以下 _locked 版假定调用者已持有 s_radio_mutex ── */

/* 夜间省电核心: 停 WiFi 射频. 假定已持锁. */
static esp_err_t wifi_radio_off_locked(void)
{
    if (s_night_sleep) return ESP_OK;      /* 已在夜间态, 幂等 */
    s_retry_count = 99;                    /* 事件处理里不再触发重连 */
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    esp_sntp_stop();                       /* 停 SNTP, 否则夜间无网仍后台发包/刷失败日志 */
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();       /* 关射频 (省电大头) */
    if (err != ESP_OK) {
        /* stop 失败: 不置夜间态, 让调用方下轮重试, 否则会"标记夜间但射频还开着"整夜耗电 */
        ESP_LOGE(TAG, "Night mode: esp_wifi_stop fail (%s), will retry", esp_err_to_name(err));
        return err;
    }
    s_night_sleep = true;                  /* 确认射频已停, 才进夜间态 */
    ESP_LOGI(TAG, "Night mode: WiFi radio stopped (power save)");
    return err;
}

/* 白天恢复核心: 重开 WiFi 射频并重连. 假定已持锁. */
static esp_err_t wifi_radio_on_locked(void)
{
    if (!s_night_sleep) return ESP_OK;     /* 本就在白天态 */
    esp_err_t err = esp_wifi_start();      /* 重开射频 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Day resume: esp_wifi_start fail (%s), will retry", esp_err_to_name(err));
        return err;                        /* 保持 s_night_sleep=true, 调用方下轮再试 */
    }
    s_night_sleep = false;
    s_retry_count = 0;
    esp_err_t cerr = esp_wifi_connect();   /* STA_START 事件也会触发, 这里再补一次无害 */
    if (cerr != ESP_OK)
        ESP_LOGW(TAG, "Day resume: esp_wifi_connect ret %s", esp_err_to_name(cerr));
    ESP_LOGI(TAG, "Day mode: WiFi radio resumed");
    return ESP_OK;
}

/* 夜间省电: 停 WiFi 射频. 公开接口, 加锁串行化, 供 api_task 调用. */
esp_err_t wifi_radio_off(void)
{
    esp_err_t err = ESP_OK;
    if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, portMAX_DELAY) == pdTRUE) {
        err = wifi_radio_off_locked();
        xSemaphoreGive(s_radio_mutex);
    }
    return err;
}

/* 白天恢复: 重开 WiFi 射频. 公开接口, 加锁串行化, 供 api_task 调用. */
esp_err_t wifi_radio_on(void)
{
    esp_err_t err = ESP_OK;
    if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, portMAX_DELAY) == pdTRUE) {
        err = wifi_radio_on_locked();
        xSemaphoreGive(s_radio_mutex);
    }
    return err;
}

/* 手动重启射频 (左键短按). 持锁内完成 off→on 原子序列, 并在锁内重判夜间:
 * 若此刻 api_task 已切入夜间省电(射频主动关), 则不重开, 避免"整夜射频被短按打开
 * 且状态机脱钩"的竞态 (原 ui_task 无锁 off()+on() 会踩这个坑). */
esp_err_t wifi_restart_radio(bool is_night_now)
{
    esp_err_t err = ESP_OK;
    if (s_radio_mutex && xSemaphoreTake(s_radio_mutex, portMAX_DELAY) == pdTRUE) {
        if (is_night_now || s_night_sleep) {
            /* 夜间: 不碰射频, 交由 api_task 的省电状态机统一管理 */
            ESP_LOGW(TAG, "restart_radio skipped (night power-save)");
            err = ESP_ERR_INVALID_STATE;
        } else {
            /* 白天: off→on 原子重启. off_locked 会置 s_night_sleep=true,
             * on_locked 再清回, 全程持锁, api_task 看不到中间态 */
            wifi_radio_off_locked();
            err = wifi_radio_on_locked();
        }
        xSemaphoreGive(s_radio_mutex);
    }
    return err;
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

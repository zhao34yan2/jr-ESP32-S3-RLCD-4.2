/**
 * @file main.cpp
 * @brief RLCD Monitor 主入口
 *
 * 基金监控 · 天气 · 黄金 · DeepSeek Token 用量
 * Board: Waveshare ESP32-S3-RLCD-4.2
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_wifi.h>

#include <esp_adc/adc_oneshot.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_softap.h>
#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"
#include "secrets.h"

#include "wifi_app.h"
#include "shtc3.h"
#include "api_clients.h"
#include "ui_main.h"
#include "ui_dashboard.h"

static const char *TAG = "MAIN";

static int g_page = 0;
static bool g_key_last = true;  /* KEY=GPIO0默认高电平 */
static lv_obj_t *main_screen = NULL;

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN,
                     RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

/* ===== 电池电量读取 (ADC1_CH3 = GPIO4) ===== */
static adc_oneshot_unit_handle_t s_adc_handle = NULL;

static int read_battery_pct(void)
{
    if (!s_adc_handle) {
        adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = ADC_UNIT_1 };
        if (adc_oneshot_new_unit(&init_cfg, &s_adc_handle) != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit init fail");
            return -1;
        }
        adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_3, &chan_cfg);
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &raw) != ESP_OK) {
        return -1;
    }
    /* Li-ion 查表映射 (ADC → 电量%), 中间插值 */
    static const int lut_adc[] = {1639,1600,1570,1540,1510,1480,1450,1420,1390,1360,1330,1300,1280};
    static const int lut_pct[] = {100, 93,  85,  75,  65,  55,  45,  35,  25,  15,  8,   3,   0};
    int pct = 0;
    if (raw >= lut_adc[0]) {
        pct = 100;
    } else if (raw <= lut_adc[12]) {
        pct = 0;
    } else {
        for (int i = 0; i < 12; i++) {
            if (raw >= lut_adc[i+1] && raw < lut_adc[i]) {
                pct = lut_pct[i] + (raw - lut_adc[i]) * (lut_pct[i+1] - lut_pct[i]) / (lut_adc[i+1] - lut_adc[i]);
                break;
            }
        }
    }
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ===== 电池电量日志 (NVS) ===== */
#define BAT_LOG_INTERVAL_MS  1800000  /* 每30分钟记一条 */
#define BAT_LOG_MAX          72       /* 保留36小时 */
#define BAT_LOG_KEY          "bat_log"

/* 日志格式: 4字节时间戳 + 1字节电量% */
typedef struct {
    uint32_t ts;     /* unix 时间戳 */
    uint8_t  pct;    /* 0~100 */
} __attribute__((packed)) BatRec_t;

static nvs_handle_t s_bat_nvs = 0;
static int32_t s_bat_log_count = 0;
static int32_t s_bat_next_idx = 0;    /* 环形缓冲区的下一个写入位置 */
static int32_t s_bat_peak_pct = -1;  /* 最高电量 (充满基准) */
static uint32_t s_bat_peak_ts = 0;
static int s_chg_start_pct = -1;     /* 充电起始电量 (用于时间估算) */
static uint32_t s_chg_start_ts = 0;
static int s_chg_reported = -1;      /* 已上报的充电电量 */

static void battery_log_init(void)
{
    esp_err_t err = nvs_open("battery", NVS_READWRITE, &s_bat_nvs);
    if (err != ESP_OK) {
        ESP_LOGW("BAT", "NVS open fail: %d", err);
        return;
    }
    nvs_get_i32(s_bat_nvs, "count", &s_bat_log_count);
    nvs_get_i32(s_bat_nvs, "next", &s_bat_next_idx);
    if (s_bat_log_count > BAT_LOG_MAX) s_bat_log_count = BAT_LOG_MAX;
    if (s_bat_next_idx >= BAT_LOG_MAX) s_bat_next_idx = 0;
    nvs_get_i32(s_bat_nvs, "peak", &s_bat_peak_pct);
    nvs_get_i32(s_bat_nvs, "peak_ts", (int32_t *)&s_bat_peak_ts);
    ESP_LOGI("BAT", "Log init: %d records, peak=%d%%", s_bat_log_count, s_bat_peak_pct);

}

static void battery_log_save(int pct)
{
    if (!s_bat_nvs) return;

    time_t now;
    time(&now);
    if (now < 100000) return; /* NTP未同步, 时间不可靠 */

    /* 读取最后一条记录的时间, 判断是否够10分钟 */
    BatRec_t last = {};
    size_t sz = sizeof(BatRec_t);
    int idx = (int)((s_bat_log_count > 0) ? s_bat_log_count - 1 : 0);
    char key[16];
    snprintf(key, sizeof(key), "rec%d", idx);
    if (nvs_get_blob(s_bat_nvs, key, &last, &sz) == ESP_OK && last.ts > 0) {
        if (now - last.ts < BAT_LOG_INTERVAL_MS / 1000) return; /* 间隔未到 */
    }

    /* 环形缓冲: 写入 next_idx, 然后推进 */
    int write_idx = (int)s_bat_next_idx;
    BatRec_t rec;
    rec.ts = (uint32_t)now;
    rec.pct = (uint8_t)pct;
    snprintf(key, sizeof(key), "rec%d", write_idx);
    nvs_set_blob(s_bat_nvs, key, &rec, sizeof(BatRec_t));

    s_bat_next_idx++;
    if (s_bat_next_idx >= BAT_LOG_MAX) s_bat_next_idx = 0;
    nvs_set_i32(s_bat_nvs, "next", (int32_t)s_bat_next_idx);

    if (s_bat_log_count < BAT_LOG_MAX) s_bat_log_count++;

    /* 更新峰值: 仅当明显上升(>=3%)才视为充电, 忽略ADC波动 */
    if (s_bat_peak_pct < 0 || pct > s_bat_peak_pct + 3) {
        s_bat_peak_pct = pct;
        s_bat_peak_ts = rec.ts;
        nvs_set_i32(s_bat_nvs, "peak", (int32_t)s_bat_peak_pct);
        nvs_set_i32(s_bat_nvs, "peak_ts", (int32_t)s_bat_peak_ts);
        ESP_LOGI("BAT", "Peak updated: %d%%", pct);
    }

    nvs_commit(s_bat_nvs);
}

static void battery_calc_trend(AppData_t *app)
{
    app->bat_log_count = (int)s_bat_log_count;

    if (s_bat_nvs && s_bat_peak_pct < 0 && s_bat_log_count > 0) {
        /* 从最后一条记录恢复峰值 (兼容旧NVS) */
        BatRec_t last = {};
        size_t sz = sizeof(BatRec_t);
        char key[16];
        snprintf(key, sizeof(key), "rec%d", (int)(s_bat_log_count - 1));
        if (nvs_get_blob(s_bat_nvs, key, &last, &sz) == ESP_OK && last.ts > 0) {
            s_bat_peak_pct = last.pct;
            s_bat_peak_ts = last.ts;
            nvs_set_i32(s_bat_nvs, "peak", (int32_t)s_bat_peak_pct);
            nvs_set_i32(s_bat_nvs, "peak_ts", (int32_t)s_bat_peak_ts);
            nvs_commit(s_bat_nvs);
            ESP_LOGI("BAT", "Peak recovered: %d%%", s_bat_peak_pct);
        }
    }

    if (s_bat_log_count < 2 || s_bat_peak_ts == 0) {
        app->bat_drop_per_h = 0;
        app->bat_est_hours = 999;
        return;
    }

    /* 用峰值对比当前 */
    uint32_t now = (uint32_t)time(NULL);
    int dropped = s_bat_peak_pct - app->battery_pct;

    /* 检测充电: 电量比上次高 = 在充电 (即使峰值更高) */
    static int s_prev_pct = -1;
    int is_charging = (dropped <= 0);
    if (!is_charging && s_prev_pct >= 0 && app->battery_pct > s_prev_pct + 2) {
        is_charging = true;  /* 电量回升超过2% = 充电 */
    }

    s_prev_pct = app->battery_pct;

    if (is_charging) {
        /* 充电中: 用插电前的电量作为起始值, 充满才跳100% */
        if (s_chg_reported < 0) {
            s_chg_reported = (s_prev_pct >= 0) ? s_prev_pct : app->battery_pct;
        }
        if (app->battery_pct >= 98) s_chg_reported = 100;
        app->bat_charge_pct = (s_chg_reported > 100) ? 100 : s_chg_reported;
        app->bat_drop_per_h = -1;
        app->bat_est_hours = 999;
        return;
    }
    /* 放电时重置充电状态 */
    s_chg_start_pct = -1;
    s_chg_reported = -1;

    float elapsed_h = (float)(now - s_bat_peak_ts) / 3600.0f;
    if (elapsed_h < 0.5f) { /* 刚拔USB不到30分钟, 数据太少 (Li-ion电压还稳定) */
        app->bat_drop_per_h = -1;
        return;
    }

    int per_h = (int)((float)dropped / elapsed_h + 0.5f);
    if (per_h < 1) per_h = 1;

    app->bat_drop_per_h = per_h;
    app->bat_est_hours = app->battery_pct / per_h;

    ESP_LOGI("BAT", "Trend: peak=%d%%, now=%d%%, drop=%d%% in %.1fh = %d%%/h",
             s_bat_peak_pct, app->battery_pct, dropped, elapsed_h, per_h);
}

/* ===== LVGL 刷新回调 ===== */
static void lvgl_flush_cb(lv_display_t *drv, const lv_area_t *area, uint8_t *color_map)
{
    uint16_t *buffer = (uint16_t *)color_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
            RlcdPort.RLCD_SetPixel(x, y, color);
            buffer++;
        }
    }
    RlcdPort.RLCD_Display();
    lv_disp_flush_ready(drv);
}

/* ===== 全局数据缓存 ===== */
static AppData_t g_app_data = {};
static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;

/* ===== 传感器读取任务 ===== */
static void sensor_task(void *pv)
{
    while (1) {
        float temp = 0, hum = 0;
        if (shtc3_read(&temp, &hum) == ESP_OK) {
            /* 合理性检查：温度 0~80°C（板子发热可能偏高），湿度 0~100% */
            if (hum >= 0 && hum <= 100) {
                taskENTER_CRITICAL(&s_data_lock);
                g_app_data.indoor_temp = temp;
                g_app_data.indoor_hum  = hum;
                taskEXIT_CRITICAL(&s_data_lock);
                ESP_LOGI(TAG, "SHTC3: %.1f°C %.1f%%RH", temp, hum);
            }
        } else {
            ESP_LOGW(TAG, "SHTC3 read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_REFRESH_SEC * 1000));
    }
}

/* ===== API 轮询任务 ===== */
static void api_task(void *pv)
{
    /* 设置负偏移让首次请求立即触发 */
    TickType_t last_weather = -pdMS_TO_TICKS(3600000); /* 立即触发 */
    TickType_t last_fund = -pdMS_TO_TICKS(3600000); /* 首次立即触发 (负>30分) */
    TickType_t last_gold = -pdMS_TO_TICKS(3600000);
/* silver removed */
    TickType_t last_ds = -pdMS_TO_TICKS(3600000);

    while (1) {
        TickType_t now = xTaskGetTickCount();
        bool wifi_ok = wifi_is_connected();
        wifi_check_reconnect();

        /* 天气 (5分钟) — 需 WiFi */
        if (wifi_ok && now - last_weather >= pdMS_TO_TICKS(WEATHER_REFRESH_SEC * 1000)) {
            last_weather = now;
            WeatherData_t tw = {};
            if (api_fetch_weather(&tw) == ESP_OK) {
                taskENTER_CRITICAL(&s_data_lock);
                g_app_data.weather = tw;
                taskEXIT_CRITICAL(&s_data_lock);
                ESP_LOGI(TAG, "Weather: %s %.1f°C", tw.condition, tw.temp_outdoor);
            } else {
                last_weather = now - pdMS_TO_TICKS(WEATHER_REFRESH_SEC * 1000) + pdMS_TO_TICKS(60000);
            }
        }

        /* 基金 (30分钟) — 需 WiFi */
        if (wifi_ok && now - last_fund >= pdMS_TO_TICKS(FUND_REFRESH_SEC * 1000)) {
            last_fund = now;
            FundItem_t tf[MAX_FUNDS];
            /* 先拷贝旧数据, 失败的基金保留旧净值 */
            taskENTER_CRITICAL(&s_data_lock);
            memcpy(tf, g_app_data.funds, sizeof(tf));
            taskEXIT_CRITICAL(&s_data_lock);
            int tc = 0;
            if (api_fetch_funds(tf, &tc) == ESP_OK) {
                taskENTER_CRITICAL(&s_data_lock);
                memcpy(g_app_data.funds, tf, sizeof(tf));
                g_app_data.fund_count = tc;
                taskEXIT_CRITICAL(&s_data_lock);
                ESP_LOGI(TAG, "Funds: %d items", tc);
            } else {
                last_fund = now - pdMS_TO_TICKS(FUND_REFRESH_SEC * 1000) + pdMS_TO_TICKS(60000);
            }
        }

        /* 黄金 (30分钟) — 需 WiFi */
        if (wifi_ok && now - last_gold >= pdMS_TO_TICKS(GOLD_REFRESH_SEC * 1000)) {
            last_gold = now;
            GoldData_t tg = {};
            if (api_fetch_gold(&tg) == ESP_OK) {
                taskENTER_CRITICAL(&s_data_lock);
                g_app_data.gold = tg;
                taskEXIT_CRITICAL(&s_data_lock);
                ESP_LOGI(TAG, "Gold: %.2f 元/克", tg.price);
            } else {
                last_gold = now - pdMS_TO_TICKS(GOLD_REFRESH_SEC * 1000) + pdMS_TO_TICKS(60000);
            }
        }

/* 白银已移除 */

        /* Bridge URL 更新 => 立即重拉数据 */
        if (g_bridge_url_changed) {
            g_bridge_url_changed = 0;
            ESP_LOGI(TAG, "Bridge URL changed, re-fetching...");
            last_fund = -pdMS_TO_TICKS(1000);
            last_ds = -pdMS_TO_TICKS(1000);
        }

        /* DeepSeek (1分钟, 通过 bridge) — 需 WiFi */
        if (wifi_ok && now - last_ds >= pdMS_TO_TICKS(DEEPSEEK_REFRESH_SEC * 1000)) {
            last_ds = now;
            DeepSeekData_t td = {};
            if (api_fetch_deepseek(&td) == ESP_OK) {
                taskENTER_CRITICAL(&s_data_lock);
                g_app_data.ds = td;
                taskEXIT_CRITICAL(&s_data_lock);
                ESP_LOGI(TAG, "DS balance: ¥%.2f", td.balance);
            } else {
                last_ds = now - pdMS_TO_TICKS(DEEPSEEK_REFRESH_SEC * 1000) + pdMS_TO_TICKS(30000);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000)); /* 每5秒检查一次 */
    }
}

/* ===== UI 更新任务 ===== */
static void ui_task(void *pv)
{
    /* 负初值让首次循环立即读取电池 */
    static TickType_t s_last_bat = -pdMS_TO_TICKS(60000);

    while (1) {
        /* KEY检测 (在锁外也可以读GPIO) */
        bool key = (gpio_get_level(KEY_GPIO) == 0);
        TickType_t now = xTaskGetTickCount();

        /* 电池电量每分钟读取一次 (ADC 频繁读取增加功耗) */
        if (now - s_last_bat >= pdMS_TO_TICKS(60000)) {
            s_last_bat = now;
            g_app_data.battery_pct = read_battery_pct();
            battery_log_save(g_app_data.battery_pct);
            battery_calc_trend(&g_app_data);
        }

        if (Lvgl_lock(100)) {
            /* KEY按下检测 (下降沿) — 手动切换页面 */
            if (key && !g_key_last) {
                g_page = !g_page;
                lv_scr_load_anim(g_page == 0 ? main_screen : dashboard_get_screen(),
                                  LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
                ESP_LOGI(TAG, "Switch to page %d", g_page);
            }
            g_key_last = key;

            /* 安全读取 (锁住数据, 快速拷贝, 释放锁) */
            AppData_t local_data;
            taskENTER_CRITICAL(&s_data_lock);
            memcpy(&local_data, &g_app_data, sizeof(AppData_t));
            taskEXIT_CRITICAL(&s_data_lock);
            if (g_page == 0)
                ui_update_all(&local_data);
            else
                dashboard_update(&local_data);

            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ===== WiFi 配网 (手机端配置) ===== */
static void prov_event_handler(void *arg, wifi_prov_cb_event_t event, void *data)
{
    switch (event) {
        case WIFI_PROV_CRED_RECV:
            ESP_LOGI(TAG, "Provisioning: credentials received");
            break;
        case WIFI_PROV_CRED_FAIL:
            ESP_LOGW(TAG, "Provisioning: credentials failed");
            wifi_prov_mgr_reset_provisioning();
            break;
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning: success, restarting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
            break;
        default:
            break;
    }
}

static void start_provisioning(void)
{
    ESP_LOGI(TAG, "Starting WiFi provisioning (AP: PROV_RLCD)");

    /* 初始化配置 */
    wifi_prov_mgr_config_t cfg = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = {
            .event_cb = prov_event_handler,
            .user_data = NULL
        },
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(cfg));

    /* 检查是否已配过网 */
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
    if (provisioned) {
        ESP_LOGI(TAG, "Already provisioned, connecting...");
        wifi_prov_mgr_deinit();
        return;
    }

    /* 启动配网 (AP 热点, 无密码, 超时 5 分钟) */
    const char *service_name = "PROV_RLCD";
    ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_0, NULL, service_name, NULL));

    /* 等待配网完成 (最长 5 分钟) */
    wifi_prov_mgr_wait();
    wifi_prov_mgr_deinit();
    ESP_LOGI(TAG, "Provisioning timeout, restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/* ===== 主入口 ===== */
/* ===== Bridge UDP 自动发现 ===== */
static void bridge_discovery_task(void *pv)
{
    /* 等待 WiFi 连接 (最多30秒) */
    for (int i = 0; i < 30; i++) {
        if (wifi_is_connected()) break;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "Discovery socket fail"); return; }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7777);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Discovery bind fail");
        close(sock);
        return;
    }

    struct timeval tv = {3, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "Bridge discovery listening on UDP:7777...");
    char last_url[128] = "";
    while (1) {
        char buf[256] = {};
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 18 && memcmp(buf, "RLCD_BRIDGE ", 12) == 0) {
            char *url = buf + 12;
            while (n > 12 && (url[n-13] == '\n' || url[n-13] == '\r')) url[--n - 12] = '\0';
            url[n - 12] = '\0';
            /* 仅当 URL 变化时才写 NVS (防 Flash 磨损) */
            if (strcmp(url, last_url) != 0) {
                strlcpy(last_url, url, sizeof(last_url));
                ESP_LOGI(TAG, "Discovery: %s", url);
                set_bridge_url(url);
            }
        }
    }
    close(sock);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== RLCD Monitor Starting ===");

    /* 显示 Bridge URL 来源 */
    ESP_LOGI(TAG, "Bridge URL: %s", get_bridge_url());

    /* 1. 初始化 NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. 初始化显示 */
    RlcdPort.RLCD_Init();
    Lvgl_PortInit(LCD_WIDTH, LCD_HEIGHT, lvgl_flush_cb);

    /* 3. 初始化 I2C (SHTC3 + RTC) */
    i2c_master_init();

    /* 立即读取一次 SHTC3 作为室温基准 (芯片还没发热) */
    {
        float t0 = 0, h0 = 0;
        if (shtc3_read(&t0, &h0) == ESP_OK) {
            shtc3_set_baseline(t0 - 20.0f);  /* 首次读数减去芯片起始发热 */
        }
    }

    /* 4. 初始化 UI */
    if (Lvgl_lock(-1)) {
        ui_init();
        main_screen = lv_scr_act();
        Lvgl_unlock();
    }

    /* 初始化KEY (GPIO0) */
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << KEY_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    g_key_last = gpio_get_level(KEY_GPIO);

    /* 5. 连接 WiFi + NTP 同步 (失败则进手机配网) */
    wifi_init_sta(WIFI_SSID, WIFI_PASSWORD);
    if (wifi_wait_connected(15000) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connected, syncing NTP...");
        ntp_sync_time();
        setenv("TZ", "CST-8", 1);
        tzset();
        ESP_LOGI(TAG, "Timezone set to CST-8 (China)");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout, starting provisioning...");
        start_provisioning();
    }

    /* 6. 初始化仪表盘页面 */
    dashboard_create();

    /* 7. 启动任务 */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(api_task,    "api",    8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,     "ui",     5120, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(bridge_discovery_task, "discovery", 4096, NULL, 1, NULL, 1);

    /* 8. 初始化电池日志 */
    battery_log_init();

    ESP_LOGI(TAG, "=== All systems running ===");
    ESP_LOGI(TAG, "Type 'bridge://IP:PORT' to change Bridge URL");
}

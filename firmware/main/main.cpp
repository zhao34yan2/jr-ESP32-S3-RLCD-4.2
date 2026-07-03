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
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_wifi.h>

#include <esp_adc/adc_oneshot.h>
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
        adc_oneshot_new_unit(&init_cfg, &s_adc_handle);
        adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_3, &chan_cfg);
    }
    int raw = 0;
    adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &raw);
    /* 3倍分压: 满电4.2V→ADC≈1737, 空电3.3V→ADC≈1364 */
    int pct = (raw - 1200) * 100 / (1639 - 1200);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* ===== 电池电量日志 (NVS) ===== */
#define BAT_LOG_INTERVAL_MS  600000   /* 每10分钟记一条 */
#define BAT_LOG_MAX          432      /* 保留3天 */
#define BAT_LOG_KEY          "bat_log"

/* 日志格式: 4字节时间戳 + 1字节电量% */
typedef struct {
    uint32_t ts;     /* unix 时间戳 */
    uint8_t  pct;    /* 0~100 */
} __attribute__((packed)) BatRec_t;

static nvs_handle_t s_bat_nvs = 0;
static int32_t s_bat_log_count = 0;
static int32_t s_bat_peak_pct = -1;  /* 最高电量 (充满基准) */
static uint32_t s_bat_peak_ts = 0;

static void battery_log_init(void)
{
    esp_err_t err = nvs_open("battery", NVS_READWRITE, &s_bat_nvs);
    if (err != ESP_OK) {
        ESP_LOGW("BAT", "NVS open fail: %d", err);
        return;
    }
    /* 读取已有记录数 */
    size_t sz = sizeof(s_bat_log_count);
    nvs_get_i32(s_bat_nvs, "count", &s_bat_log_count);
    if (s_bat_log_count > BAT_LOG_MAX) s_bat_log_count = BAT_LOG_MAX;
    /* 读取第一条数据用于趋势计算 */
    /* 读最高记录作为放电基准 */
    nvs_get_i32(s_bat_nvs, "peak", &s_bat_peak_pct);
    nvs_get_i32(s_bat_nvs, "peak_ts", (int32_t *)&s_bat_peak_ts);
    ESP_LOGI("BAT", "Log init: %d records, peak=%d%%", s_bat_log_count, s_bat_peak_pct);

    /* 导出日志用于分析 */
    if (s_bat_log_count > 0) {
        ESP_LOGI("BAT", "=== Battery Log Dump (ts, pct) ===");
        for (int i = 0; i < s_bat_log_count; i++) {
            char k[16];
            snprintf(k, sizeof(k), "rec%d", i);
            BatRec_t r = {};
            size_t rs = sizeof(BatRec_t);
            if (nvs_get_blob(s_bat_nvs, k, &r, &rs) == ESP_OK && r.ts > 0) {
                ESP_LOGI("BAT", "%d,%d", r.ts, r.pct);
            }
        }
        ESP_LOGI("BAT", "=== End ===");
    }
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

    /* 环形写入 */
    int write_idx = (int)s_bat_log_count;
    if (write_idx >= BAT_LOG_MAX) {
        /* 满了, 覆盖最早一条, 整体前移 */
        for (int i = 1; i < BAT_LOG_MAX; i++) {
            char k_old[16], k_new[16];
            snprintf(k_old, sizeof(k_old), "rec%d", i);
            snprintf(k_new, sizeof(k_new), "rec%d", i - 1);
            BatRec_t tmp = {};
            size_t tsz = sizeof(BatRec_t);
            if (nvs_get_blob(s_bat_nvs, k_old, &tmp, &tsz) == ESP_OK) {
                nvs_set_blob(s_bat_nvs, k_new, &tmp, sizeof(BatRec_t));
            }
        }
        write_idx = BAT_LOG_MAX - 1;
        s_bat_log_count = BAT_LOG_MAX;
    } else {
        s_bat_log_count++;
    }

    BatRec_t rec;
    rec.ts = (uint32_t)now;
    rec.pct = (uint8_t)pct;
    snprintf(key, sizeof(key), "rec%d", write_idx);
    nvs_set_blob(s_bat_nvs, key, &rec, sizeof(BatRec_t));
    nvs_set_i32(s_bat_nvs, "count", (int32_t)s_bat_log_count);

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

    if (dropped <= 0) {
        /* 电池等于或高于峰值: 充电中 */
        app->bat_drop_per_h = -1;
        app->bat_est_hours = 999;
        return;
    }

    float elapsed_h = (float)(now - s_bat_peak_ts) / 3600.0f;
    if (elapsed_h < 0.1f) { /* 刚拔USB不到6分钟, 数据太少 */
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

/* ===== 传感器读取任务 ===== */
static void sensor_task(void *pv)
{
    while (1) {
        float temp = 0, hum = 0;
        if (shtc3_read(&temp, &hum) == ESP_OK) {
            /* 合理性检查：温度 0~80°C（板子发热可能偏高），湿度 0~100% */
            if (hum >= 0 && hum <= 100) {
                g_app_data.indoor_temp = temp;
                g_app_data.indoor_hum  = hum;
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
            if (api_fetch_weather(&g_app_data.weather) == ESP_OK) {
                ESP_LOGI(TAG, "Weather: %s %.1f°C", g_app_data.weather.condition,
                         g_app_data.weather.temp_outdoor);
            } else {
                last_weather -= pdMS_TO_TICKS(60000); /* 失败后1分钟重试 */
            }
        }

        /* 基金 (30分钟) — 需 WiFi */
        if (wifi_ok && now - last_fund >= pdMS_TO_TICKS(FUND_REFRESH_SEC * 1000)) {
            last_fund = now;
            if (api_fetch_funds(g_app_data.funds, &g_app_data.fund_count) == ESP_OK) {
                ESP_LOGI(TAG, "Funds: %d items", g_app_data.fund_count);
                api_calc_summary(&g_app_data);
            } else {
                last_fund -= pdMS_TO_TICKS(30000);
            }
        }

        /* 黄金 (30分钟) — 需 WiFi */
        if (wifi_ok && now - last_gold >= pdMS_TO_TICKS(GOLD_REFRESH_SEC * 1000)) {
            last_gold = now;
            if (api_fetch_gold(&g_app_data.gold) == ESP_OK) {
                ESP_LOGI(TAG, "Gold: %.2f 元/克", g_app_data.gold.price);
            } else {
                last_gold -= pdMS_TO_TICKS(30000);
            }
        }

/* 白银已移除 */

        /* DeepSeek (1分钟, 通过 bridge) — 需 WiFi */
        if (wifi_ok && now - last_ds >= pdMS_TO_TICKS(DEEPSEEK_REFRESH_SEC * 1000)) {
            last_ds = now;
            if (api_fetch_deepseek(&g_app_data.ds) == ESP_OK) {
                ESP_LOGI(TAG, "DS balance: ¥%.2f", g_app_data.ds.balance);
            } else {
                last_ds -= pdMS_TO_TICKS(30000);
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

            /* 更新当前页面数据 */
            if (g_page == 0)
                ui_update_all(&g_app_data);
            else
                dashboard_update(&g_app_data);

            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ===== 主入口 ===== */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== RLCD Monitor Starting ===");

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

    /* 5. 连接 WiFi + NTP 同步 */
    wifi_init_sta(WIFI_SSID, WIFI_PASSWORD);
    if (wifi_wait_connected(15000) == ESP_OK) {
        ESP_LOGI(TAG, "WiFi connected, syncing NTP...");
        ntp_sync_time();
        setenv("TZ", "CST-8", 1);
        tzset();
        ESP_LOGI(TAG, "Timezone set to CST-8 (China)");
    } else {
        ESP_LOGW(TAG, "WiFi connection timeout, will retry later");
    }

    /* 6. 初始化仪表盘页面 */
    dashboard_create();

    /* 7. 启动任务 */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(api_task,    "api",    8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,     "ui",     5120, NULL, 6, NULL, 0);

    /* 8. 初始化电池日志 */
    battery_log_init();

    ESP_LOGI(TAG, "=== All systems running ===");
}
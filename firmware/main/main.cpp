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
    int pct = (raw - 1100) * 100 / (1650 - 1100);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
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
static AppData_t g_app_data = {0};

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

        /* 电池电量每分钟读取一次 (ADC 频繁读取增加功耗) */
        TickType_t now = xTaskGetTickCount();
        if (now - s_last_bat >= pdMS_TO_TICKS(60000)) {
            s_last_bat = now;
            g_app_data.battery_pct = read_battery_pct();
        }

        if (Lvgl_lock(100)) {
            /* KEY按下检测 (下降沿) */
            if (key && !g_key_last) {
                g_page = !g_page;
                if (g_page == 0)
                    lv_scr_load(main_screen);
                else
                    lv_scr_load(dashboard_get_screen());
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
    gpio_config_t io_conf = {0};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << KEY_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
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

    ESP_LOGI(TAG, "=== All systems running ===");
}
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
#include <freertos/semphr.h>
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
        if (adc_oneshot_new_unit(&init_cfg, &s_adc_handle) != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit init fail");
            return -1;
        }
        adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_3, &chan_cfg);
    }
    /* 多次采样求平均, 抑制 ADC 噪声 (单次读数会跳变几十 LSB) */
    int raw = 0;
    {
        int sum = 0, valid = 0;
        for (int k = 0; k < 16; k++) {
            int r = 0;
            if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &r) == ESP_OK) {
                sum += r;
                valid++;
            }
        }
        if (valid == 0) return -1;
        raw = sum / valid;
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

/* ===== 电池电量趋势 (纯 RAM, 不落 NVS) =====
 * 旧版把 72 条记录反复写 16KB 的 NVS, 碎片化后 nvs_set 返回 NOT_ENOUGH_SPACE(0x1105),
 * 导致配网 WiFi 凭证存不进去. 趋势属非关键数据, 改为纯 RAM (重启重置), 彻底不占 NVS.
 * 电量% 由 ADC 直接读, 不受影响. */
#define BAT_LOG_INTERVAL_MS  600000   /* 每10分钟采一次趋势 (>=2 次才出趋势, 即约 20 分钟后) */
#define BAT_LOG_MAX          72

static int32_t s_bat_log_count = 0;  /* 已采样次数 (>=2 才出趋势) */
static int s_bat_peak_pct = -1;      /* 最高电量 (充满基准) */
static uint32_t s_bat_peak_ts = 0;
static int s_chg_reported = -1;      /* 已上报的充电电量 */

/* 开机一次性清理旧版遗留的 battery NVS 命名空间, 回收被撑碎的空间 */
static void battery_nvs_cleanup(void)
{
    nvs_handle_t h;
    if (nvs_open("battery", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI("BAT", "old battery NVS cleared (reclaim space)");
    }
}

static void battery_log_save(int pct)
{
    time_t now;
    time(&now);
    if (now < 100000) return; /* NTP未同步, 时间不可靠 */

    /* 距上次采样够间隔才更新 (RAM 计时, 不落 NVS) */
    static uint32_t s_last_ts = 0;
    if (s_last_ts && (uint32_t)now - s_last_ts < BAT_LOG_INTERVAL_MS / 1000) return;
    s_last_ts = (uint32_t)now;

    if (s_bat_log_count < BAT_LOG_MAX) s_bat_log_count++;

    /* 更新峰值: 仅当明显上升(>=3%)才视为充电, 忽略ADC波动 */
    if (s_bat_peak_pct < 0 || pct > s_bat_peak_pct + 3) {
        s_bat_peak_pct = pct;
        s_bat_peak_ts = (uint32_t)now;
    }
}

static void battery_calc_trend(AppData_t *app)
{
    app->bat_log_count = (int)s_bat_log_count;

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
/* 数据锁: 用 mutex 替代 portMUX (spinlock+关中断),
 * 避免拷贝大结构体时长时间关中断影响实时性 */
static SemaphoreHandle_t s_data_mutex = NULL;
#define DATA_LOCK()   do { if (s_data_mutex) xSemaphoreTake(s_data_mutex, portMAX_DELAY); } while (0)
#define DATA_UNLOCK() do { if (s_data_mutex) xSemaphoreGive(s_data_mutex); } while (0)

/* ===== 传感器读取任务 ===== */
static void sensor_task(void *pv)
{
    while (1) {
        float temp = 0, hum = 0;
        if (shtc3_read(&temp, &hum) == ESP_OK) {
            /* 合理性检查：温度 0~80°C（板子发热可能偏高），湿度 0~100% */
            if (hum >= 0 && hum <= 100) {
                DATA_LOCK();
                g_app_data.indoor_temp = temp;
                g_app_data.indoor_hum  = hum;
                DATA_UNLOCK();
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
    /* NTP 每 24h 重新同步一次, 防止晶振长期漂移 (开机已同步过, 故初值=now) */
    TickType_t last_ntp = xTaskGetTickCount();

    while (1) {
        TickType_t now = xTaskGetTickCount();
        bool wifi_ok = wifi_is_connected();
        wifi_check_reconnect();

        /* 每 24 小时重新对时 — 需 WiFi */
        if (wifi_ok && now - last_ntp >= pdMS_TO_TICKS(24 * 3600 * 1000)) {
            last_ntp = now;
            ESP_LOGI(TAG, "24h NTP re-sync...");
            ntp_sync_time();
        }

        /* 天气 (5分钟) — 需 WiFi */
        if (wifi_ok && now - last_weather >= pdMS_TO_TICKS(WEATHER_REFRESH_SEC * 1000)) {
            last_weather = now;
            WeatherData_t tw = {};
            if (api_fetch_weather(&tw) == ESP_OK) {
                DATA_LOCK();
                g_app_data.weather = tw;
                DATA_UNLOCK();
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
            DATA_LOCK();
            memcpy(tf, g_app_data.funds, sizeof(tf));
            DATA_UNLOCK();
            int tc = 0;
            if (api_fetch_funds(tf, &tc) == ESP_OK) {
                DATA_LOCK();
                memcpy(g_app_data.funds, tf, sizeof(tf));
                g_app_data.fund_count = tc;
                DATA_UNLOCK();
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
                DATA_LOCK();
                g_app_data.gold = tg;
                DATA_UNLOCK();
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

        /* DeepSeek (5分钟, 直连 HTTPS 回退 bridge) — 需 WiFi */
        if (wifi_ok && now - last_ds >= pdMS_TO_TICKS(DEEPSEEK_REFRESH_SEC * 1000)) {
            last_ds = now;
            DeepSeekData_t td = {};
            if (api_fetch_deepseek(&td) == ESP_OK) {
                DATA_LOCK();
                g_app_data.ds = td;
                DATA_UNLOCK();
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
    /* 显示内容重算节流: 循环 50ms 保证按键跟手, 但内容每 5 秒才重算一次
     * (时钟只到分, 5 秒粒度足够). 配合 set_label 只在文本变化时刷新, 静止时几乎不重绘. */
    TickType_t s_last_draw = 0;
    bool first_draw = true;

    while (1) {
        /* KEY检测 (在锁外也可以读GPIO) */
        bool key = (gpio_get_level(KEY_GPIO) == 0);
        TickType_t now = xTaskGetTickCount();

        /* 电池电量每分钟读取一次 (ADC 频繁读取增加功耗) */
        if (now - s_last_bat >= pdMS_TO_TICKS(60000)) {
            s_last_bat = now;
            /* 在临时结构上计算, 算完加锁写回, 避免与 UI 读取竞争 */
            AppData_t bat_tmp;
            DATA_LOCK();
            memcpy(&bat_tmp, &g_app_data, sizeof(AppData_t));
            DATA_UNLOCK();
            bat_tmp.battery_pct = read_battery_pct();
            battery_log_save(bat_tmp.battery_pct);
            battery_calc_trend(&bat_tmp);
            DATA_LOCK();
            g_app_data.battery_pct   = bat_tmp.battery_pct;
            g_app_data.bat_drop_per_h = bat_tmp.bat_drop_per_h;
            g_app_data.bat_est_hours  = bat_tmp.bat_est_hours;
            g_app_data.bat_log_count  = bat_tmp.bat_log_count;
            g_app_data.bat_charge_pct = bat_tmp.bat_charge_pct;
            DATA_UNLOCK();
        }

        if (Lvgl_lock(100)) {
            /* KEY按下检测 (下降沿) — 手动切换页面, 切换后强制立即重绘 */
            bool page_switched = false;
            if (key && !g_key_last) {
                g_page = !g_page;
                lv_scr_load_anim(g_page == 0 ? main_screen : dashboard_get_screen(),
                                  LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
                ESP_LOGI(TAG, "Switch to page %d", g_page);
                page_switched = true;
            }
            g_key_last = key;

            /* 内容每 5 秒重算一次 (或切页/首次立即重算); 时钟已只到分, 5 秒粒度足够,
             * set_label 再按需刷新, 文本没变则整屏不重绘 */
            if (first_draw || page_switched || now - s_last_draw >= pdMS_TO_TICKS(5000)) {
                s_last_draw = now;
                first_draw = false;
                /* 安全读取 (锁住数据, 快速拷贝, 释放锁) */
                AppData_t local_data;
                DATA_LOCK();
                memcpy(&local_data, &g_app_data, sizeof(AppData_t));
                DATA_UNLOCK();
                if (g_page == 0)
                    ui_update_all(&local_data);
                else
                    dashboard_update(&local_data);
            }

            Lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ===== WiFi 配网 (AP 模式 + 网页配置) ===== */
/* DNS 服务器 (Captive Portal: 所有域名指向 192.168.4.1) */
static void dns_task(void *pv)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr = { .s_addr = htonl(INADDR_ANY) } };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return; }
    uint8_t buf[512];
    while (1) {
        struct sockaddr_in from; socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
        if (n > 12 && (buf[2] & 0x80) == 0) {
            buf[2] = 0x81; buf[3] = 0x80; buf[7] = 1;
            int q = 12; while (q < n && buf[q] != 0) q++;
            q += 5;
            if (q + 16 <= (int)sizeof(buf)) {
                buf[q] = 0xC0; buf[q+1] = 0x0C;
                buf[q+2] = 0; buf[q+3] = 1;  buf[q+4] = 0; buf[q+5] = 1;
                buf[q+6] = 0; buf[q+7] = 0; buf[q+8] = 0; buf[q+9] = 60;
                buf[q+10] = 0; buf[q+11] = 4;
                buf[q+12] = 192; buf[q+13] = 168; buf[q+14] = 4; buf[q+15] = 1;
                sendto(sock, buf, q + 16, 0, (struct sockaddr *)&from, flen);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* WiFi 扫描 (返回可选网络列表) */
/* URL 解码 */
static void url_decode(char *s) {
    char *d = s;
    while (*s) {
        if (*s == '+') { *d++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            int v = 0;
            for (int i = 1; i <= 2; i++) {
                char c = s[i];
                v = v * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
            }
            *d++ = v; s += 3;
        } else { *d++ = *s++; }
    }
    *d = '\0';
}

/* 发送完整 HTTP 响应 (header 用 strlen 计算, 不写死长度) */
static void http_send(int c, const char *status, const char *ctype, const char *body)
{
    char hdr[128];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %s\r\nContent-Type: %s\r\nConnection: close\r\n\r\n",
        status, ctype);
    write(c, hdr, hlen);
    if (body) write(c, body, strlen(body));
}

/* 302 跳转到配置门户 (用于 captive portal 探测) */
static void http_redirect_portal(int c)
{
    const char *hdr =
        "HTTP/1.0 302 Found\r\n"
        "Location: http://192.168.4.1/\r\n"
        "Connection: close\r\n\r\n";
    write(c, hdr, strlen(hdr));
}

/* 提取 POST body 里某个表单字段 (application/x-www-form-urlencoded) */
static bool form_get(const char *body, const char *key, char *out, size_t out_sz)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '&' && *p != ' ' && *p != '\r' && *p != '\n' && i < out_sz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    url_decode(out);   /* 解码 %XX 和 + */
    return true;
}

/* URL 编码 (percent-encoding): 让 SSID 里的空格/特殊字符原样往返, 不被 '_' 替换污染 */
static void url_encode(char *dst, size_t dstsz, const char *src)
{
    static const char *hex = "0123456789ABCDEF";
    size_t d = 0;
    for (const unsigned char *s = (const unsigned char *)src; *s && d + 3 < dstsz; s++) {
        unsigned char c = *s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = (char)c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[(c >> 4) & 0xF];
            dst[d++] = hex[c & 0xF];
        }
    }
    dst[d] = '\0';
}

/* HTML 转义: 放进属性/文本时防止 ' " < > & 破坏页面 (显示名 + value 预填都用) */
static void html_escape(char *dst, size_t dstsz, const char *src)
{
    size_t d = 0;
    for (const char *s = src; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
            case '&': rep = "&amp;";  break;
            case '<': rep = "&lt;";   break;
            case '>': rep = "&gt;";   break;
            case '\'':rep = "&#39;";  break;
            case '"': rep = "&quot;"; break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            if (d + rl >= dstsz) break;
            memcpy(dst + d, rep, rl); d += rl;
        } else {
            if (d + 1 >= dstsz) break;
            dst[d++] = *s;
        }
    }
    dst[d] = '\0';
}

/* 简易 HTTP 服务器 (原始 socket, 不依赖 esp_http_server) */
static void http_task(void *pv)
{
    int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv < 0) return;
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(80), .sin_addr = { .s_addr = htonl(INADDR_ANY) } };
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(srv); return; }
    listen(srv, 3);
    while (1) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int c = accept(srv, (struct sockaddr *)&cli, &clen);
        if (c < 0) continue;
        /* 每个连接设读超时, 防止半开连接阻塞 */
        struct timeval tv = {5, 0};
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* static: http 任务单线程串行处理, 避免大数组占栈导致溢出 */
        static char buf[1536];
        memset(buf, 0, sizeof(buf));
        /* 健壮读取: header 与 body 常分片到达 (甚至 Expect:100-continue),
         * 只读一次会丢掉 POST 表单 body → 配置存不进去. 先读到完整 header, 再按
         * Content-Length 补齐 body. */
        int total = 0;
        while (total < (int)sizeof(buf) - 1) {
            int r = read(c, buf + total, sizeof(buf) - 1 - total);
            if (r <= 0) break;
            total += r;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;   /* header 读完 */
        }
        if (total > 0) {
            /* 客户端若声明 Expect: 100-continue, 先回 100 它才会发送 body */
            if (strstr(buf, "100-continue") || strstr(buf, "100-Continue")) {
                const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
                write(c, cont, strlen(cont));
            }
            /* POST: 按 Content-Length 把 body 收齐 (表单字段就在 body 里) */
            char *hdr_end = strstr(buf, "\r\n\r\n");
            if (hdr_end) {
                const char *cl = strstr(buf, "Content-Length:");
                if (!cl) cl = strstr(buf, "content-length:");
                int content_len = cl ? atoi(cl + 15) : 0;
                int body_have = total - (int)((hdr_end + 4) - buf);
                while (body_have < content_len && total < (int)sizeof(buf) - 1) {
                    int r = read(c, buf + total, sizeof(buf) - 1 - total);
                    if (r <= 0) break;
                    total += r;
                    buf[total] = '\0';
                    body_have = total - (int)((hdr_end + 4) - buf);
                }
            }

            /* Captive Portal 探测: 手机连上开放热点后会请求这些 URL 判断是否有网,
             * 统一 302 跳到门户, 手机才会自动弹出配置页并保持连接 */
            if (strstr(buf, "generate_204") || strstr(buf, "gen_204") ||
                strstr(buf, "hotspot-detect") || strstr(buf, "connectivitycheck") ||
                strstr(buf, "ncsi.txt") || strstr(buf, "connecttest")) {
                http_redirect_portal(c);
                close(c);
                continue;
            }

            if (strstr(buf, "GET /scan")) {
                /* 扫描 WiFi (需 APSTA 模式, 纯 AP 下扫描会失败) */
                uint16_t cnt = 16;
                static wifi_ap_record_t rec[16];
                esp_err_t se = esp_wifi_scan_start(NULL, true);
                static char body[4096]; int pos = 0;
                pos += snprintf(body + pos, sizeof(body) - pos,
                    "<html><head><meta charset=utf-8></head><body><h2>选择 WiFi</h2>");
                if (se == ESP_OK && esp_wifi_scan_get_ap_records(&cnt, rec) == ESP_OK) {
                    if (cnt > 16) cnt = 16;
                    for (int i = 0; i < cnt; i++) {
                        if (rec[i].ssid[0] == '\0') continue;
                        /* href 用 URL 编码 (原样往返), 显示用 HTML 转义 (防破坏页面) */
                        char enc[128], disp[208];
                        url_encode(enc, sizeof(enc), (const char *)rec[i].ssid);
                        html_escape(disp, sizeof(disp), (const char *)rec[i].ssid);
                        pos += snprintf(body + pos, sizeof(body) - pos,
                            "<a href='/?s=%s' style='display:block;padding:8px;border:1px solid #ddd;text-decoration:none;color:#333'>%s</a>",
                            enc, disp);
                    }
                } else {
                    pos += snprintf(body + pos, sizeof(body) - pos,
                        "<p>扫描失败, 请手动输入 WiFi 名称</p>");
                }
                snprintf(body + pos, sizeof(body) - pos, "<br><a href='/'>返回</a></body></html>");
                http_send(c, "200 OK", "text/html", body);
            } else if (strstr(buf, "POST /save")) {
                /* 保存配置: 只覆盖 cfg 命名空间的 ssid/pass, 不擦除其它 NVS 数据 */
                char ssid[33] = "", pass[65] = "";
                bool has_ssid = form_get(buf, "ssid", ssid, sizeof(ssid));
                form_get(buf, "pass", pass, sizeof(pass));
                bool saved = false;
                if (has_ssid && ssid[0]) {
                    nvs_handle_t nv;
                    if (nvs_open("cfg", NVS_READWRITE, &nv) == ESP_OK) {
                        /* 先把当前主网络降为备用槽 (不同网络才备份, 避免重复写) */
                        char cur[33] = {};
                        size_t csz = sizeof(cur);
                        if (nvs_get_str(nv, "ssid", cur, &csz) == ESP_OK &&
                            cur[0] && strcmp(cur, ssid) != 0) {
                            char curp[65] = {};
                            csz = sizeof(curp);
                            nvs_get_str(nv, "pass", curp, &csz);
                            nvs_set_str(nv, "ssid2", cur);
                            nvs_set_str(nv, "pass2", curp);
                            ESP_LOGI(TAG, "Backup prev WiFi: %s", cur);
                        }
                        esp_err_t e1 = nvs_set_str(nv, "ssid", ssid);
                        esp_err_t e2 = nvs_set_str(nv, "pass", pass);
                        esp_err_t ce = nvs_commit(nv);
                        nvs_close(nv);
                        /* 三者都成功才算存好; set 失败(如分区满)时 commit 仍返回 0 */
                        saved = (e1 == ESP_OK && e2 == ESP_OK && ce == ESP_OK);
                        ESP_LOGI(TAG, "Saved WiFi cfg: ssid='%s' pass_len=%d set=%d/%d commit=%d",
                                 ssid, (int)strlen(pass), e1, e2, ce);
                    } else {
                        ESP_LOGE(TAG, "POST /save: nvs_open cfg fail");
                    }
                } else {
                    ESP_LOGW(TAG, "POST /save: ssid 解析失败 (body 未收全?), total=%d", total);
                }
                if (saved) {
                    /* 只有真正存成功才重启, 避免"假成功"重启进坏状态 */
                    http_send(c, "200 OK", "text/html",
                        "<html><head><meta charset=utf-8></head><body>"
                        "<h2>已保存, 正在重启...</h2></body></html>");
                    close(c); close(srv);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    esp_restart();
                    return;
                }
                /* 保存失败: 回错误页让用户重试, 不重启 */
                http_send(c, "200 OK", "text/html",
                    "<html><head><meta charset=utf-8></head><body>"
                    "<h2>保存失败, 请返回重试</h2><a href='/'>返回</a></body></html>");
            } else {
                /* 首页 (含根路径和被劫持的其它域名) */
                char sel[64] = "";
                const char *q = strstr(buf, "?s=");
                if (q) { q += 3; int i = 0; while (*q && *q != ' ' && *q != '&' && i < 63) sel[i++] = *q++; sel[i] = '\0'; url_decode(sel); }
                char sel_esc[208];
                html_escape(sel_esc, sizeof(sel_esc), sel);   /* 预填进 value='' 前转义, 防 ' 破坏属性 */
                static char body[4096];
                snprintf(body, sizeof(body),
                    "<html><head><meta charset=utf-8><meta name=viewport content='width=320,initial-scale=1'>"
                    "<style>body{font:14px sans-serif;margin:16px;text-align:center}"
                    "input,button{width:90%%;padding:8px;margin:4px;border:1px solid #ccc;border-radius:4px;box-sizing:border-box}"
                    "button{background:#2d7;color:#fff;border:none;font-size:16px;cursor:pointer}</style>"
                    "</head><body><h2>RLCD 配置</h2>"
                    "<a href='/scan' style='display:inline-block;width:90%%;padding:8px;margin:4px;background:#2d7;color:#fff;border-radius:4px;text-decoration:none;font-size:16px;box-sizing:border-box'>扫描 WiFi</a>"
                    "<form action=/save method=post>"
                    "<input name=ssid placeholder='WiFi 名称' value='%s'><br>"
                    "<input type=password name=pass placeholder='WiFi 密码'><br>"
                    "<button>保存并重启</button>"
                    "</form></body></html>", sel_esc);
                http_send(c, "200 OK", "text/html", body);
            }
        }
        close(c);
    }
    close(srv);
}

/* 启动 AP 配网模式 */
static void start_ap_provision(void)
{
    ESP_LOGI(TAG, "Starting AP provision...");
    /* 先停掉 STA 自动重连, 否则重连与 WiFi 扫描抢射频, 导致扫描卡死/时好时坏 */
    wifi_stop_sta_reconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (!ap) { ESP_LOGE(TAG, "AP netif fail"); return; }
    /* APSTA: STA 接口保留才能扫描 WiFi (纯 AP 模式无法 scan) */
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t wc = {};
    memcpy(wc.ap.ssid, "RLCD-AP", 7);
    wc.ap.ssid_len = 7;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_wifi_start();
    ESP_LOGI(TAG, "AP 'RLCD-AP' started");

    /* DNS 劫持 + HTTP 服务器 */
    xTaskCreate(dns_task, "dns", 4096, NULL, 3, NULL);
    xTaskCreate(http_task, "http", 12288, NULL, 3, NULL);
    ESP_LOGI(TAG, "Services started");

    /* 等待 5 分钟 */
    for (int i = 0; i < 300; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Timeout, restarting...");
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
    ESP_LOGI(TAG, "nvs_flash_init: %d", ret);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        ESP_LOGI(TAG, "nvs_flash_init after erase: %d", ret);
    }
    ESP_ERROR_CHECK(ret);

    /* 清理旧版电池日志占用的 NVS (回收被撑碎的空间, 让配网凭证能写入) */
    battery_nvs_cleanup();
    {
        nvs_stats_t nvst;
        if (nvs_get_stats(NULL, &nvst) == ESP_OK)
            ESP_LOGI(TAG, "NVS: used=%d free=%d", (int)nvst.used_entries, (int)nvst.free_entries);
    }

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

    /* 5. 连接 WiFi, 失败则进 AP 配网模式 */
    /* 优先读取配网页面存入 NVS 的凭证 (ssid=主, ssid2=上次用过的备用); 读不到再回退 secrets.h */
    char sta_ssid[33] = WIFI_SSID;
    char sta_pass[65] = WIFI_PASSWORD;
    char fallback_ssid[33] = {};
    char fallback_pass[65] = {};
    {
        nvs_handle_t nv;
        if (nvs_open("cfg", NVS_READONLY, &nv) == ESP_OK) {
            size_t sz = sizeof(sta_ssid);
            char tmp[33] = {};
            if (nvs_get_str(nv, "ssid", tmp, &sz) == ESP_OK && tmp[0]) {
                strlcpy(sta_ssid, tmp, sizeof(sta_ssid));
                sz = sizeof(sta_pass);
                if (nvs_get_str(nv, "pass", sta_pass, &sz) != ESP_OK) sta_pass[0] = '\0';
                ESP_LOGI(TAG, "Primary WiFi from NVS: %s", sta_ssid);
            }
            /* 备用槽: 上次用过的网络 (换网时自动降级) */
            sz = sizeof(fallback_ssid);
            char tmp2[33] = {};
            if (nvs_get_str(nv, "ssid2", tmp2, &sz) == ESP_OK && tmp2[0] &&
                strcmp(tmp2, sta_ssid) != 0) {   /* 不与主网络相同才试 */
                strlcpy(fallback_ssid, tmp2, sizeof(fallback_ssid));
                sz = sizeof(fallback_pass);
                if (nvs_get_str(nv, "pass2", fallback_pass, &sz) != ESP_OK) fallback_pass[0] = '\0';
                ESP_LOGI(TAG, "Fallback WiFi from NVS: %s", fallback_ssid);
            }
            nvs_close(nv);
        }
    }
    wifi_init_sta(sta_ssid, sta_pass);
    bool wifi_ok = (wifi_wait_connected(20000) == ESP_OK);
    if (!wifi_ok && fallback_ssid[0]) {
        /* 主网络连不上, 试备用 (不重建驱动, 直接切换配置) */
        ESP_LOGW(TAG, "Primary failed, trying fallback: %s", fallback_ssid);
        wifi_switch_network(fallback_ssid, fallback_pass);
        wifi_ok = (wifi_wait_connected(20000) == ESP_OK);
    }
    if (wifi_ok) {
        ESP_LOGI(TAG, "WiFi connected!");
        ntp_sync_time();
        setenv("TZ", "CST-8", 1);
        tzset();
    } else {
        ESP_LOGW(TAG, "WiFi failed, AP mode");
        start_ap_provision();
    }

    /* 6. 初始化仪表盘页面 (持 LVGL 锁: 此时 LVGL 任务已在运行, 避免创建对象与渲染竞争) */
    if (Lvgl_lock(-1)) {
        dashboard_create();
        Lvgl_unlock();
    }

    /* 7. 创建数据锁 (必须在任务启动前) */
    s_data_mutex = xSemaphoreCreateMutex();
    if (!s_data_mutex) ESP_LOGE(TAG, "data mutex create fail");

    /* 8. 启动任务 */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(api_task,    "api",    8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,     "ui",     5120, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(bridge_discovery_task, "discovery", 4096, NULL, 1, NULL, 1);

    ESP_LOGI(TAG, "=== All systems running ===");
    ESP_LOGI(TAG, "Type 'bridge://IP:PORT' to change Bridge URL");
}

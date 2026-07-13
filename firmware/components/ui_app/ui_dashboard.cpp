/**
 * @file ui_dashboard.cpp — 四宫格桌面屏保
 *
 * 字体: custom_font_14_big (等线 14px, 1bpp)
 * 与 ui_main.cpp 完全相同的字体声明方式
 */

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "ui_dashboard.h"
#include "wifi_app.h"
#include "secrets.h"

static const char *TAG = "DASH";

#define C_BLACK lv_color_black()
#define C_WHITE lv_color_white()

/* 与 ui_main.cpp 完全一致的字体声明 — 不用 extern "C" */
LV_FONT_DECLARE(custom_font_14_big);
#define FONT_CN  (&custom_font_14_big)

/* ===== 所有标签句柄 ===== */
static lv_obj_t *scr;
static lv_obj_t *l_date, *l_wkd, *l_cond, *l_temp, *l_city;
static lv_obj_t *l_itemp, *l_ihum, *l_ibat, *l_iwifi;
static lv_obj_t *l_fc[3];
static lv_obj_t *l_d1, *l_d2, *l_d3, *l_d4;

/* ===== 辅助: 创建标签 ===== */
static lv_obj_t *mk(lv_obj_t *p, const char *t, lv_coord_t x, lv_coord_t y, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, C_BLACK, 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

/* ===== 辅助: 创建带边框卡片 ===== */
static lv_obj_t *card(lv_obj_t *p, const char *title,
                       lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_border_color(c, C_BLACK, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_bg_color(c, C_WHITE, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, x, y);

    lv_obj_t *tl = lv_label_create(c);
    lv_label_set_text(tl, title);
    lv_obj_set_style_text_color(tl, C_BLACK, 0);
    lv_obj_set_style_text_font(tl, FONT_CN, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *ln = lv_obj_create(c);
    lv_obj_set_size(ln, w - 8, 1);
    lv_obj_set_style_bg_color(ln, C_BLACK, 0);
    lv_obj_set_style_border_width(ln, 0, 0);
    lv_obj_align(ln, LV_ALIGN_TOP_LEFT, 4, 18);

    return c;
}

/* ===== 创建 ===== */
void dashboard_create(void)
{
    /* 设置字体回退 (与 ui_main.cpp 完全一致) */
    ((lv_font_t *)(FONT_CN))->fallback = &lv_font_montserrat_14;

    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);

    /* 卡片尺寸 (无顶部状态栏) */
    const lv_coord_t CW = 190, CH = 130;
    const lv_coord_t X1 = 4, X2 = X1 + CW + 8;
    const lv_coord_t Y1 = 4, Y2 = Y1 + CH + 6;

    /* ── 天气卡 ── */
    {
        lv_obj_t *c = card(scr, "天气", X1, Y1, CW, CH);
        l_date = mk(c, "----/--/--", 4, 22, &lv_font_montserrat_14);
        l_wkd  = mk(c, "---", 4, 40, FONT_CN);
        l_cond = mk(c, "--", 4, 58, FONT_CN);
        l_temp = mk(c, "--", 4, 76, &lv_font_montserrat_14);
        l_city = mk(c, "---", 4, 98, FONT_CN);
    }

    /* ── 室内卡 ── */
    {
        lv_obj_t *c = card(scr, "室内", X2, Y1, CW, CH);
        l_itemp = mk(c, "温度: --.-C", 4, 22, FONT_CN);
        l_ihum  = mk(c, "湿度: --%", 4, 44, FONT_CN);
        l_ibat  = mk(c, "电池: ---", 4, 66, FONT_CN);
        l_iwifi = mk(c, "WiFi: ---", 4, 88, FONT_CN);
    }

    /* ── 预报卡 ── */
    {
        lv_obj_t *c = card(scr, "预报", X1, Y2, CW, CH);
        for (int i = 0; i < 3; i++)
            l_fc[i] = mk(c, "---", 4, 22 + i * 20, FONT_CN);
    }

    /* ── 设备卡 ── */
    {
        lv_obj_t *c = card(scr, "设备", X2, Y2, CW, CH);
        l_d1 = mk(c, "---", 4, 22, &lv_font_montserrat_12);
        l_d2 = mk(c, "---", 4, 44, &lv_font_montserrat_12);
        l_d3 = mk(c, "---", 4, 66, FONT_CN);
        l_d4 = mk(c, "---", 4, 88, &lv_font_montserrat_12);
    }

    ESP_LOGI(TAG, "4-grid (custom_font_14_big)");
}

/* ===== 更新 ===== */
void dashboard_update(const AppData_t *app)
{
    char b[80];
    time_t t;
    struct tm ti;
    time(&t);
    localtime_r(&t, &ti);

    /* 天气卡 */
    snprintf(b, sizeof b, "%04d/%02d/%02d", ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
    lv_label_set_text(l_date, b);
    static const char *wd[] = {"日","一","二","三","四","五","六"};
    snprintf(b, sizeof b, "星期%s", wd[ti.tm_wday]);
    lv_label_set_text(l_wkd, b);
    if (app->weather.condition[0]) lv_label_set_text(l_cond, app->weather.condition);
    if (app->weather.temp_max > -50) {
        snprintf(b, sizeof b, "%.0f-%.0fC", app->weather.temp_min, app->weather.temp_max);
        lv_label_set_text(l_temp, b);
    }
    lv_label_set_text(l_city, WEATHER_CITY);

    /* 室内卡 */
    snprintf(b, sizeof b, "温度: %.1fC", app->indoor_temp);
    lv_label_set_text(l_itemp, b);
    snprintf(b, sizeof b, "湿度: %d%%", (int)app->indoor_hum);
    lv_label_set_text(l_ihum, b);
    snprintf(b, sizeof b, "电池: %d%%", app->battery_pct);
    lv_label_set_text(l_ibat, b);
    snprintf(b, sizeof b, "WiFi: %s", wifi_is_connected() ? "OK" : "NO");
    lv_label_set_text(l_iwifi, b);

    /* 预报卡 */
    int nd = app->weather.fc_count > 3 ? 3 : app->weather.fc_count;
    for (int i = 0; i < nd; i++) {
        struct tm fd = ti;
        fd.tm_mday += i;
        mktime(&fd);
        snprintf(b, sizeof b, "%02d/%02d-%s %.0f-%.0fC",
                 fd.tm_mon+1, fd.tm_mday,
                 app->weather.fc_cond[i],
                 app->weather.fc_min[i], app->weather.fc_max[i]);
        lv_label_set_text(l_fc[i], b);
    }

    /* 设备卡 — 简洁监控 */
    {
        esp_chip_info_t ci;
        esp_chip_info(&ci);

        /* 总内存 (内部 RAM + PSRAM) */
        size_t r_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL)
                       + heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        size_t r_free  = esp_get_free_heap_size();
        int pct = (r_total > 0) ? (int)(r_free * 100 / r_total) : 0;

        snprintf(b, sizeof b, "CPU: %dMHz  Free:%d%%",
                 CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, pct);
        lv_label_set_text(l_d1, b);

        size_t r_used = r_total - r_free;
        snprintf(b, sizeof b, "RAM: %.1f/%.1fM",
                 (double)r_used / (1024*1024), (double)r_total / (1024*1024));
        lv_label_set_text(l_d2, b);

        /* 电量趋势 (替代PSRAM行) */
        if (app->bat_log_count < 2) {
            snprintf(b, sizeof b, "电耗:记录中..");
        } else if (app->bat_drop_per_h > 0) {
            snprintf(b, sizeof b, "电耗:%d%%/h %dh",
                     app->bat_drop_per_h, app->bat_est_hours);
        } else if (app->bat_drop_per_h < 0) {
            snprintf(b, sizeof b, "Charging..");
        } else {
            snprintf(b, sizeof b, "电耗:%d%%/h", 1);
        }
        lv_label_set_text(l_d3, b);

        uint32_t ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        snprintf(b, sizeof b, "Up: %uh%02um",
                 (unsigned)(ms/3600000), (unsigned)((ms%3600000)/60000));
        lv_label_set_text(l_d4, b);
    }

}

lv_obj_t *dashboard_get_screen(void)
{
    return scr;
}
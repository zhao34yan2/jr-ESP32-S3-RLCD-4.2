/**
 * @file ui_dashboard.cpp — 第二屏: 天气主题 (仿 234.png)
 *
 * 布局 (400x300):
 * ┌─ 状态栏 (时间/日期/周几 · 信号/电量) ───────────── 24px
 * ├─ 中部双栏 ─────────────────────────────────── 150px
 * │  当前天气(大温度+图标+体感/湿度/风/降水) │ 室内(温/湿/电/WiFi)
 * ├─ 7天预报 (7列: 周几/日期/图标/高/低) ──────────  剩余
 * └──────────────────────────────────────────────
 *
 * 字体: custom_font_14_big (等线 14px 中文) + font_num_40 (大温度) + Montserrat
 * 天气图标: weather_icons.c (A8 位图, 大/小两套)
 */

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <lvgl.h>
#include "ui_dashboard.h"
#include "wifi_app.h"
#include "secrets.h"

static const char *TAG = "DASH";

#define C_BLACK lv_color_black()
#define C_WHITE lv_color_white()

LV_FONT_DECLARE(custom_font_14_big);
LV_FONT_DECLARE(font_num_40);
#define FONT_CN   (&custom_font_14_big)
#define FONT_NUM  (&font_num_40)

/* 天气图标 (A8), 由 gen_weather_icons.py 生成 */
LV_IMAGE_DECLARE(wi_sun_big);   LV_IMAGE_DECLARE(wi_sun_sm);
LV_IMAGE_DECLARE(wi_partly_big);LV_IMAGE_DECLARE(wi_partly_sm);
LV_IMAGE_DECLARE(wi_cloud_big); LV_IMAGE_DECLARE(wi_cloud_sm);
LV_IMAGE_DECLARE(wi_fog_big);   LV_IMAGE_DECLARE(wi_fog_sm);
LV_IMAGE_DECLARE(wi_rain_big);  LV_IMAGE_DECLARE(wi_rain_sm);
LV_IMAGE_DECLARE(wi_heavyrain_big); LV_IMAGE_DECLARE(wi_heavyrain_sm);
LV_IMAGE_DECLARE(wi_snow_big);  LV_IMAGE_DECLARE(wi_snow_sm);

/* WMO 天气码 → 图标 (big=当前, sm=预报) */
static const lv_image_dsc_t *code_icon(int code, bool big)
{
    switch (code) {
        case 0: case 1:            return big ? &wi_sun_big    : &wi_sun_sm;
        case 2:                    return big ? &wi_partly_big : &wi_partly_sm;
        case 3:                    return big ? &wi_cloud_big  : &wi_cloud_sm;
        case 45: case 48:          return big ? &wi_fog_big    : &wi_fog_sm;
        case 51: case 53: case 55:
        case 56: case 57:
        case 61: case 66: case 67:
        case 80:                   return big ? &wi_rain_big   : &wi_rain_sm;
        case 63: case 65:
        case 81: case 82:          return big ? &wi_heavyrain_big : &wi_heavyrain_sm;
        case 71: case 73: case 75:
        case 77: case 85: case 86: return big ? &wi_snow_big   : &wi_snow_sm;
        case 95: case 96: case 99: return big ? &wi_heavyrain_big : &wi_heavyrain_sm;
        default:                   return big ? &wi_cloud_big  : &wi_cloud_sm;
    }
}

/* ===== 控件句柄 ===== */
static lv_obj_t *scr;
/* 状态栏 */
static lv_obj_t *l_time, *l_date, *l_batt;
static lv_obj_t *sig_bar[4], *batt_box, *batt_fill;
/* 当前天气 */
static lv_obj_t *l_cur_temp, *l_cur_loc, *l_cur_cond, *cur_ico;
static lv_obj_t *l_feel, *l_hum, *l_wind, *l_prec;
/* 室内 */
static lv_obj_t *l_in_temp, *l_in_hum, *l_in_bat, *l_in_wifi;
/* 预报 7 列 */
static lv_obj_t *fc_wk[7], *fc_dt[7], *fc_ic[7], *fc_hi[7], *fc_lo[7];
static int fc_last_code[7] = {-1,-1,-1,-1,-1,-1,-1};

/* 仅在文本变化时才 set (反射屏静止零重绘) */
static void set_label(lv_obj_t *l, const char *txt)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(l, txt);
}

/* 通用: 创建标签 */
static lv_obj_t *mk(lv_obj_t *p, const char *t, int x, int y, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, C_BLACK, 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

/* 竖直/水平分隔线 */
static void hline(lv_obj_t *p, int x, int y, int w)
{
    lv_obj_t *l = lv_obj_create(p);
    lv_obj_set_size(l, w, 1);
    lv_obj_set_style_bg_color(l, C_BLACK, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
}
static void vline(lv_obj_t *p, int x, int y, int h)
{
    lv_obj_t *l = lv_obj_create(p);
    lv_obj_set_size(l, 1, h);
    lv_obj_set_style_bg_color(l, C_BLACK, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
}

/* 区块标题左侧的实心竖条 (仿 234.png 的 ▊) */
static void title_bar(lv_obj_t *p, int x, int y)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_set_size(b, 3, 13);
    lv_obj_set_style_bg_color(b, C_BLACK, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
}

/* 布局常量 */
#define STATUS_H  24
#define MID_Y     (STATUS_H + 1)
#define MID_H     150
#define CUR_W     232
#define FC_Y      (MID_Y + MID_H + 1)

void dashboard_create(void)
{
    ((lv_font_t *)FONT_CN)->fallback = &lv_font_montserrat_14;

    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ===== ① 状态栏 ===== */
    l_time = mk(scr, "--:--", 5, 4, &lv_font_montserrat_16);
    l_date = mk(scr, "--月--日 周-", 70, 6, FONT_CN);

    /* 电量数字 + 电池框 (右) */
    l_batt = mk(scr, "--%", 330, 5, &lv_font_montserrat_14);
    lv_obj_align(l_batt, LV_ALIGN_TOP_RIGHT, -26, 5);
    batt_box = lv_obj_create(scr);
    lv_obj_set_size(batt_box, 22, 12);
    lv_obj_set_style_bg_color(batt_box, C_WHITE, 0);
    lv_obj_set_style_border_color(batt_box, C_BLACK, 0);
    lv_obj_set_style_border_width(batt_box, 1, 0);
    lv_obj_set_style_radius(batt_box, 1, 0);
    lv_obj_set_style_pad_all(batt_box, 2, 0);
    lv_obj_set_scrollbar_mode(batt_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(batt_box, LV_ALIGN_TOP_RIGHT, -2, 6);
    batt_fill = lv_obj_create(batt_box);
    lv_obj_set_style_bg_color(batt_fill, C_BLACK, 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_set_style_radius(batt_fill, 0, 0);
    lv_obj_align(batt_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(batt_fill, 16, 8);

    /* 信号 4 格 (电量左侧) */
    for (int i = 0; i < 4; i++) {
        sig_bar[i] = lv_obj_create(scr);
        int h = 3 + i * 3;
        lv_obj_set_size(sig_bar[i], 3, h);
        lv_obj_set_style_bg_color(sig_bar[i], C_BLACK, 0);
        lv_obj_set_style_border_width(sig_bar[i], 0, 0);
        lv_obj_set_style_radius(sig_bar[i], 0, 0);
        lv_obj_align(sig_bar[i], LV_ALIGN_TOP_RIGHT, -70 + i * 5, 6 + (11 - h));
    }

    hline(scr, 0, STATUS_H, 400);

    /* ===== ② 中部双栏 ===== */
    vline(scr, CUR_W, MID_Y, MID_H);
    hline(scr, 0, MID_Y + MID_H, 400);

    /* --- 左: 当前天气 --- */
    title_bar(scr, 5, MID_Y + 6);
    mk(scr, "当前", 11, MID_Y + 3, FONT_CN);
    mk(scr, "CURRENT", 48, MID_Y + 5, &lv_font_montserrat_12);
    l_cur_loc = mk(scr, "建邺·南京", CUR_W - 90, MID_Y + 3, FONT_CN);
    lv_label_set_long_mode(l_cur_loc, LV_LABEL_LONG_CLIP);

    l_cur_temp = mk(scr, "--", 8, MID_Y + 22, FONT_NUM);
    l_cur_cond = mk(scr, "--", 10, MID_Y + 74, FONT_CN);

    cur_ico = lv_image_create(scr);
    lv_image_set_src(cur_ico, &wi_cloud_big);
    lv_obj_align(cur_ico, LV_ALIGN_TOP_LEFT, CUR_W - 92, MID_Y + 26);

    hline(scr, 8, MID_Y + 96, CUR_W - 16);

    /* 体感/湿度/风/降水: 两列 */
    int gx1 = 10, gx2 = 120, gy = MID_Y + 102, gyr = 22;
    mk(scr, "体感", gx1, gy, FONT_CN);
    l_feel = mk(scr, "--°", gx1 + 38, gy, FONT_CN);
    mk(scr, "湿度", gx2, gy, FONT_CN);
    l_hum  = mk(scr, "--%", gx2 + 38, gy, FONT_CN);
    mk(scr, "风", gx1, gy + gyr, FONT_CN);
    l_wind = mk(scr, "--级", gx1 + 38, gy + gyr, FONT_CN);
    mk(scr, "降水", gx2, gy + gyr, FONT_CN);
    l_prec = mk(scr, "--%", gx2 + 38, gy + gyr, FONT_CN);

    /* --- 右: 室内 --- */
    int ix = CUR_W + 8;
    title_bar(scr, ix, MID_Y + 6);
    mk(scr, "室内", ix + 6, MID_Y + 3, FONT_CN);
    mk(scr, "INDOOR", ix + 44, MID_Y + 5, &lv_font_montserrat_12);

    int iy = MID_Y + 30, ir = 30;
    mk(scr, "温度", ix + 2, iy, FONT_CN);
    l_in_temp = mk(scr, "--°C", ix + 2, iy, FONT_CN);
    lv_obj_align(l_in_temp, LV_ALIGN_TOP_RIGHT, -8, iy);
    mk(scr, "湿度", ix + 2, iy + ir, FONT_CN);
    l_in_hum = mk(scr, "--%", ix + 2, iy + ir, FONT_CN);
    lv_obj_align(l_in_hum, LV_ALIGN_TOP_RIGHT, -8, iy + ir);
    mk(scr, "电池", ix + 2, iy + ir * 2, FONT_CN);
    l_in_bat = mk(scr, "--%", ix + 2, iy + ir * 2, FONT_CN);
    lv_obj_align(l_in_bat, LV_ALIGN_TOP_RIGHT, -8, iy + ir * 2);
    mk(scr, "WiFi", ix + 2, iy + ir * 3, &lv_font_montserrat_14);
    l_in_wifi = mk(scr, "--", ix + 2, iy + ir * 3, &lv_font_montserrat_16);
    lv_obj_align(l_in_wifi, LV_ALIGN_TOP_RIGHT, -8, iy + ir * 3);

    /* ===== ③ 7天预报 ===== */
    title_bar(scr, 5, FC_Y + 5);
    mk(scr, "7天预报", 11, FC_Y + 2, FONT_CN);
    mk(scr, "FORECAST", 78, FC_Y + 4, &lv_font_montserrat_12);

    int col_w = 400 / 7;
    int row_y = FC_Y + 22;
    for (int i = 0; i < 7; i++) {
        int cx = i * col_w + col_w / 2;
        fc_wk[i] = mk(scr, "--", 0, row_y, FONT_CN);
        lv_obj_set_style_text_align(fc_wk[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_wk[i], col_w);
        lv_obj_align(fc_wk[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y);

        fc_dt[i] = mk(scr, "--/--", 0, row_y + 16, &lv_font_montserrat_10);
        lv_obj_set_style_text_align(fc_dt[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_dt[i], col_w);
        lv_obj_align(fc_dt[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y + 16);

        fc_ic[i] = lv_image_create(scr);
        lv_image_set_src(fc_ic[i], &wi_cloud_sm);
        lv_obj_align(fc_ic[i], LV_ALIGN_TOP_LEFT, cx - 21, row_y + 30);

        fc_hi[i] = mk(scr, "--°", 0, row_y + 66, FONT_CN);
        lv_obj_set_style_text_align(fc_hi[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_hi[i], col_w);
        lv_obj_align(fc_hi[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y + 66);

        fc_lo[i] = mk(scr, "--°", 0, row_y + 82, &lv_font_montserrat_12);
        lv_obj_set_style_text_align(fc_lo[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_lo[i], col_w);
        lv_obj_align(fc_lo[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y + 82);
    }

    ESP_LOGI(TAG, "weather dashboard created");
}

void dashboard_update(const AppData_t *app)
{
    char b[48];
    time_t t;
    struct tm ti;
    time(&t);
    localtime_r(&t, &ti);

    /* ① 状态栏 */
    snprintf(b, sizeof b, "%02d:%02d", ti.tm_hour, ti.tm_min);
    set_label(l_time, b);
    static const char *wd[] = {"日","一","二","三","四","五","六"};
    snprintf(b, sizeof b, "%d月%d日 周%s", ti.tm_mon + 1, ti.tm_mday, wd[ti.tm_wday]);
    set_label(l_date, b);
    snprintf(b, sizeof b, "%d%%", app->battery_pct);
    set_label(l_batt, b);
    int fw = app->battery_pct * 16 / 100;
    if (fw < 1) fw = 1;
    lv_obj_set_width(batt_fill, fw);

    /* 信号格: 按 RSSI 点亮 (未连接全灭) */
    int rssi = wifi_get_rssi();
    int bars = 0;
    if (wifi_is_connected() && rssi < 0) {
        if (rssi >= -60) bars = 4;
        else if (rssi >= -70) bars = 3;
        else if (rssi >= -80) bars = 2;
        else bars = 1;
    }
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_bg_opa(sig_bar[i], i < bars ? LV_OPA_COVER : LV_OPA_30, 0);

    /* ② 当前天气 */
    const WeatherData_t *w = &app->weather;
    if (w->condition[0]) {
        snprintf(b, sizeof b, "%.0f°C", w->temp_outdoor);
        set_label(l_cur_temp, b);
        set_label(l_cur_cond, w->condition);
        lv_image_set_src(cur_ico, code_icon(w->code, true));

        snprintf(b, sizeof b, "%.0f°", w->apparent_temp);
        set_label(l_feel, b);
        snprintf(b, sizeof b, "%d%%", w->humidity);
        set_label(l_hum, b);
        snprintf(b, sizeof b, "%d级", w->wind_level);
        set_label(l_wind, b);
        snprintf(b, sizeof b, "%d%%", w->precip_prob);
        set_label(l_prec, b);
    }

    /* ③ 室内 */
    snprintf(b, sizeof b, "%.1f°C", app->indoor_temp);
    set_label(l_in_temp, b);
    snprintf(b, sizeof b, "%d%%RH", (int)app->indoor_hum);
    set_label(l_in_hum, b);
    snprintf(b, sizeof b, "%d%%", app->battery_pct);
    set_label(l_in_bat, b);
    set_label(l_in_wifi, wifi_is_night_sleep() ? "Zzz"
                       : (wifi_is_connected() ? "OK" : "NO"));

    /* ④ 7天预报 */
    int nd = w->fc_count < 7 ? w->fc_count : 7;
    for (int i = 0; i < nd; i++) {
        struct tm fd = ti;
        fd.tm_mday += i;
        mktime(&fd);
        snprintf(b, sizeof b, "周%s", wd[fd.tm_wday]);
        set_label(fc_wk[i], b);
        snprintf(b, sizeof b, "%d/%d", fd.tm_mon + 1, fd.tm_mday);
        set_label(fc_dt[i], b);
        if (w->fc_code[i] != fc_last_code[i]) {
            lv_image_set_src(fc_ic[i], code_icon(w->fc_code[i], false));
            fc_last_code[i] = w->fc_code[i];
        }
        snprintf(b, sizeof b, "%.0f°", w->fc_max[i]);
        set_label(fc_hi[i], b);
        snprintf(b, sizeof b, "%.0f°", w->fc_min[i]);
        set_label(fc_lo[i], b);
    }
}

lv_obj_t *dashboard_get_screen(void)
{
    return scr;
}

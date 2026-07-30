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
 * 字体: custom_font_14_big (等线 14px 中文) + font_num_28 (大温度) + Montserrat
 * 通用控件 (ui_mk/ui_hline/ui_vline/ui_set_label/ui_sig_set/ui_wmo_icon) 来自 ui_common.
 * title_bar / 电量框 / 信号格为本屏特有几何, 保留本地绘制.
 */

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <lvgl.h>
#include "ui_dashboard.h"
#include "ui_common.h"
#include "wifi_app.h"
#include "secrets.h"

static const char *TAG = "DASH";

#define C_BLACK lv_color_black()
#define C_WHITE lv_color_white()

/* custom_font_14_big: 全量 GB2312 (等线 14px 1bpp), 含 ASCII + 度符号, 任何地名/天气词都不缺字. */
LV_FONT_DECLARE(custom_font_14_big);
LV_FONT_DECLARE(font_num_28);
#define FONT_CN   (&custom_font_14_big)
#define FONT_NUM  (&font_num_28)

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
static int cur_last_code = -1;   /* 当前天气图标: 仅 code 变化才换 (原每次 set 会触发整屏刷) */

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
#define MID_H     138
#define CUR_W     232
#define FC_Y      (MID_Y + MID_H + 1)

void dashboard_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ===== ① 状态栏 ===== */
    l_time = ui_mk(scr, "--:--", 5, 4, &lv_font_montserrat_16);
    l_date = ui_mk(scr, "--月--日 周-", 70, 6, FONT_CN);

    /* 电量数字 + 电池框 (右) */
    l_batt = ui_mk(scr, "--%", 330, 5, &lv_font_montserrat_14);
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
        lv_obj_align(sig_bar[i], LV_ALIGN_TOP_RIGHT, -84 + i * 5, 6 + (11 - h));
    }

    ui_hline(scr, 0, STATUS_H, 400, 1);

    /* ===== ② 中部双栏 ===== */
    ui_vline(scr, CUR_W, MID_Y, MID_H, 1);
    ui_hline(scr, 0, MID_Y + MID_H, 400, 1);

    /* --- 左: 当前天气 --- */
    title_bar(scr, 5, MID_Y + 6);
    ui_mk(scr, "当前", 11, MID_Y + 3, FONT_CN);
    ui_mk(scr, "CURRENT", 48, MID_Y + 5, &lv_font_montserrat_12);
    l_cur_loc = ui_mk(scr, "建邺·" WEATHER_CITY, CUR_W - 90, MID_Y + 3, FONT_CN);
    lv_label_set_long_mode(l_cur_loc, LV_LABEL_LONG_CLIP);

    l_cur_temp = ui_mk(scr, "--", 8, MID_Y + 32, FONT_NUM);
    l_cur_cond = ui_mk(scr, "--", 10, MID_Y + 66, FONT_CN);

    cur_ico = lv_image_create(scr);
    lv_image_set_src(cur_ico, ui_wmo_icon(3, true));   /* 默认多云占位 */
    lv_obj_align(cur_ico, LV_ALIGN_TOP_LEFT, CUR_W - 92, MID_Y + 26);

    ui_hline(scr, 8, MID_Y + 86, CUR_W - 16, 1);

    /* 体感/湿度/风/降水: 两列 */
    int gx1 = 10, gx2 = 120, gy = MID_Y + 92, gyr = 22;
    ui_mk(scr, "体感", gx1, gy, FONT_CN);
    l_feel = ui_mk(scr, "--°", gx1 + 38, gy, FONT_CN);
    ui_mk(scr, "湿度", gx2, gy, FONT_CN);
    l_hum  = ui_mk(scr, "--%", gx2 + 38, gy, FONT_CN);
    ui_mk(scr, "风力", gx1, gy + gyr, FONT_CN);
    l_wind = ui_mk(scr, "--级", gx1 + 38, gy + gyr, FONT_CN);
    ui_mk(scr, "降水", gx2, gy + gyr, FONT_CN);
    l_prec = ui_mk(scr, "--%", gx2 + 38, gy + gyr, FONT_CN);

    /* --- 右: 室内 --- */
    int ix = CUR_W + 8;
    ui_mk(scr, "室内", ix + 2, MID_Y + 3, FONT_CN);

    int iy = MID_Y + 28, ir = 27;
    /* 值标签: 固定宽度 + 文字右对齐, 四个值右边缘严格对齐到室内栏内侧(留8px) */
    int val_w = 400 - ix - 8;   /* 室内栏内宽 */
    ui_mk(scr, "温度", ix + 2, iy, FONT_CN);
    l_in_temp = ui_mk(scr, "--°C", ix, iy, FONT_CN);
    lv_obj_set_width(l_in_temp, val_w);
    lv_obj_set_style_text_align(l_in_temp, LV_TEXT_ALIGN_RIGHT, 0);
    ui_mk(scr, "湿度", ix + 2, iy + ir, FONT_CN);
    l_in_hum = ui_mk(scr, "--%", ix, iy + ir, FONT_CN);
    lv_obj_set_width(l_in_hum, val_w);
    lv_obj_set_style_text_align(l_in_hum, LV_TEXT_ALIGN_RIGHT, 0);
    ui_mk(scr, "电池", ix + 2, iy + ir * 2, FONT_CN);
    l_in_bat = ui_mk(scr, "--%", ix, iy + ir * 2, FONT_CN);
    lv_obj_set_width(l_in_bat, val_w);
    lv_obj_set_style_text_align(l_in_bat, LV_TEXT_ALIGN_RIGHT, 0);
    ui_mk(scr, "WiFi", ix + 2, iy + ir * 3, FONT_CN);
    l_in_wifi = ui_mk(scr, "--", ix, iy + ir * 3, FONT_CN);
    lv_obj_set_width(l_in_wifi, val_w);
    lv_obj_set_style_text_align(l_in_wifi, LV_TEXT_ALIGN_RIGHT, 0);

    /* ===== ③ 7天预报 ===== */
    title_bar(scr, 5, FC_Y + 5);
    ui_mk(scr, "7天预报", 11, FC_Y + 2, FONT_CN);
    ui_mk(scr, "FORECAST", 78, FC_Y + 4, &lv_font_montserrat_12);

    int col_w = 400 / 7;
    int row_y = FC_Y + 18;
    for (int i = 0; i < 7; i++) {
        int cx = i * col_w + col_w / 2;
        fc_wk[i] = ui_mk(scr, "--", 0, row_y, FONT_CN);
        lv_obj_set_style_text_align(fc_wk[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_wk[i], col_w);
        lv_obj_align(fc_wk[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y);

        fc_dt[i] = ui_mk(scr, "--/--", 0, row_y + 16, &lv_font_montserrat_10);
        lv_obj_set_style_text_align(fc_dt[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_dt[i], col_w);
        lv_obj_align(fc_dt[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y + 16);

        fc_ic[i] = lv_image_create(scr);
        lv_image_set_src(fc_ic[i], ui_wmo_icon(3, false));   /* 默认多云占位 */
        lv_obj_align(fc_ic[i], LV_ALIGN_TOP_LEFT, cx - 21, row_y + 30);

        fc_hi[i] = ui_mk(scr, "--°", 0, row_y + 66, FONT_CN);
        lv_obj_set_style_text_align(fc_hi[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(fc_hi[i], col_w);
        lv_obj_align(fc_hi[i], LV_ALIGN_TOP_LEFT, i * col_w, row_y + 66);

        fc_lo[i] = ui_mk(scr, "--°", 0, row_y + 82, &lv_font_montserrat_12);
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
    ui_set_label(l_time, b);
    static const char *wd[] = {"日","一","二","三","四","五","六"};
    snprintf(b, sizeof b, "%d月%d日 周%s", ti.tm_mon + 1, ti.tm_mday, wd[ti.tm_wday]);
    ui_set_label(l_date, b);
    snprintf(b, sizeof b, "%d%%", app->battery_pct);
    ui_set_label(l_batt, b);
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
    /* 1-bit 无灰阶: 熄灭格用全透明而非 30%(会被二值化抖动), 与一/三屏统一 */
    ui_sig_set(sig_bar, 4, bars);

    /* ② 当前天气 */
    const WeatherData_t *w = &app->weather;
    if (w->condition[0]) {
        snprintf(b, sizeof b, "%.0f°C", w->temp_outdoor);
        ui_set_label(l_cur_temp, b);
        ui_set_label(l_cur_cond, w->condition);
        if (w->code != cur_last_code) {
            lv_image_set_src(cur_ico, ui_wmo_icon(w->code, true));
            cur_last_code = w->code;
        }

        snprintf(b, sizeof b, "%.0f°", w->apparent_temp);
        ui_set_label(l_feel, b);
        snprintf(b, sizeof b, "%d%%", w->humidity);
        ui_set_label(l_hum, b);
        snprintf(b, sizeof b, "%d级", w->wind_level);
        ui_set_label(l_wind, b);
        snprintf(b, sizeof b, "%d%%", w->precip_prob);
        ui_set_label(l_prec, b);
    }

    /* ③ 室内 */
    snprintf(b, sizeof b, "%.1f°C", app->indoor_temp);
    ui_set_label(l_in_temp, b);
    snprintf(b, sizeof b, "%d%% RH", (int)app->indoor_hum);
    ui_set_label(l_in_hum, b);
    snprintf(b, sizeof b, "%d%%", app->battery_pct);
    ui_set_label(l_in_bat, b);
    ui_set_label(l_in_wifi, wifi_is_night_sleep() ? "省电"
                       : (wifi_is_connected() ? "已连接" : "未连接"));

    /* ④ 7天预报 */
    int nd = w->fc_count < 7 ? w->fc_count : 7;
    for (int i = 0; i < nd; i++) {
        struct tm fd = ti;
        fd.tm_mday += i;
        mktime(&fd);
        snprintf(b, sizeof b, "周%s", wd[fd.tm_wday]);
        ui_set_label(fc_wk[i], b);
        snprintf(b, sizeof b, "%d/%d", fd.tm_mon + 1, fd.tm_mday);
        ui_set_label(fc_dt[i], b);
        if (w->fc_code[i] != fc_last_code[i]) {
            lv_image_set_src(fc_ic[i], ui_wmo_icon(w->fc_code[i], false));
            fc_last_code[i] = w->fc_code[i];
        }
        snprintf(b, sizeof b, "%.0f°", w->fc_max[i]);
        ui_set_label(fc_hi[i], b);
        snprintf(b, sizeof b, "%.0f°", w->fc_min[i]);
        ui_set_label(fc_lo[i], b);
    }
}

lv_obj_t *dashboard_get_screen(void)
{
    return scr;
}

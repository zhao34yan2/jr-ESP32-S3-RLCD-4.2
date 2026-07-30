/**
 * @file ui_common.cpp
 * @brief 三屏共用控件工厂 + WMO 图标映射 (实现)
 */

#include <cstring>
#include "ui_common.h"

void ui_set_label(lv_obj_t *l, const char *txt)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, txt) == 0) return;   /* 文本没变则不刷新 */
    lv_label_set_text(l, txt);
}

lv_obj_t *ui_mk(lv_obj_t *p, const char *t, int x, int y, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, UI_BLACK, 0);
    if (f) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

lv_obj_t *ui_card(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, UI_WHITE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 6, 0);
    lv_obj_set_style_border_color(c, UI_BLACK, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* 分隔线 = 一个无边框/无圆角的实心黑矩形 */
static void ui_line(lv_obj_t *p, int x, int y, int w, int h)
{
    lv_obj_t *l = lv_obj_create(p);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_bg_color(l, UI_BLACK, 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_radius(l, 0, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
}

void ui_hline(lv_obj_t *p, int x, int y, int w, int th) { ui_line(p, x, y, w, th); }
void ui_vline(lv_obj_t *p, int x, int y, int h, int th) { ui_line(p, x, y, th, h); }

void ui_sig_set(lv_obj_t **bars, int n, int lit)
{
    for (int i = 0; i < n; i++)
        if (bars[i])
            lv_obj_set_style_bg_opa(bars[i], i < lit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/* 天气图标 (A8), 大/小两套 — 由 gen_weather_icons.py 生成到 weather_icons.c */
LV_IMAGE_DECLARE(wi_sun_big);       LV_IMAGE_DECLARE(wi_sun_sm);
LV_IMAGE_DECLARE(wi_partly_big);    LV_IMAGE_DECLARE(wi_partly_sm);
LV_IMAGE_DECLARE(wi_cloud_big);     LV_IMAGE_DECLARE(wi_cloud_sm);
LV_IMAGE_DECLARE(wi_fog_big);       LV_IMAGE_DECLARE(wi_fog_sm);
LV_IMAGE_DECLARE(wi_rain_big);      LV_IMAGE_DECLARE(wi_rain_sm);
LV_IMAGE_DECLARE(wi_heavyrain_big); LV_IMAGE_DECLARE(wi_heavyrain_sm);
LV_IMAGE_DECLARE(wi_snow_big);      LV_IMAGE_DECLARE(wi_snow_sm);

/* WMO 码 → 图标 (与 open-meteo weather_code 对应, 归并到已有的 7 套图标) */
const lv_image_dsc_t *ui_wmo_icon(int code, bool big)
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

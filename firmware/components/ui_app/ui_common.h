#ifndef UI_COMMON_H
#define UI_COMMON_H

/**
 * @file ui_common.h
 * @brief 三屏共用的 LVGL 控件工厂 + WMO 天气码→图标映射
 *
 * 硬件同三屏: 4.2" 全反射 1-bit 单色屏, 只有纯黑/纯白, 无灰阶.
 * 原先 ui_main / ui_dashboard / ui_sysinfo 各自复制了一份 set_label/mk/card/
 * 分隔线/图标映射, 改一处样式要改三遍. 统一收拢到此处.
 */

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 颜色 — RLCD 1-bit 只有黑白 */
#define UI_BLACK  lv_color_black()
#define UI_WHITE  lv_color_white()

/* 仅在文本变化时刷新 (反射屏静止时零重绘, 省电且减少刷屏闪动) */
void ui_set_label(lv_obj_t *l, const char *txt);

/* 建标签: 黑字, 可选字体 (f=NULL 用默认), 左上对齐到 (x,y) */
lv_obj_t *ui_mk(lv_obj_t *p, const char *t, int x, int y, const lv_font_t *f);

/* 圆角卡片外框: 白底 + 2px 黑框 + 圆角 6, 关滚动 */
lv_obj_t *ui_card(lv_obj_t *p, int x, int y, int w, int h);

/* 实心黑分隔线, 线宽 th px (反射屏细线会糊, 主界面用 2px) */
void ui_hline(lv_obj_t *p, int x, int y, int w, int th);
void ui_vline(lv_obj_t *p, int x, int y, int h, int th);

/* 点亮信号格前 lit 格 (实心), 其余透明. n=格子总数 */
void ui_sig_set(lv_obj_t **bars, int n, int lit);

/* WMO 天气码 → 天气图标 (big=true 当前大图, false 预报小图).
 * weather_icons.c 由 gen_weather_icons.py 生成. */
const lv_image_dsc_t *ui_wmo_icon(int code, bool big);

#ifdef __cplusplus
}
#endif

#endif /* UI_COMMON_H */

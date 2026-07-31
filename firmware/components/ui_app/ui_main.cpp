/**
 * @file ui_main.cpp
 * @brief 第一屏 — HUD 卡片式主界面 (天气/黄金/基金/DeepSeek)
 *
 * 硬件: 4.2" 全反射 1-bit 单色屏 (400×300 横用), 无背光. 设计约束同第三屏:
 *   ① 只有纯黑/纯白, 无灰阶 —— 层次靠字号/边框区分, 不用半透明淡色;
 *   ② 细线在反射屏会糊 —— 边框/分隔线 ≥2px;
 *   ③ 静止最省电 —— set_label 只在变化时刷新.
 *
 * 通用控件 (ui_mk/ui_card/ui_hline/ui_set_label/ui_sig_set/ui_wmo_icon) 来自 ui_common.
 * 状态栏电池框/信号格为本屏特有几何, 保留本地绘制.
 */

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <lvgl.h>
#include "ui_main.h"
#include "ui_common.h"
#include "wifi_app.h"
#include "api_clients.h"
#include "secrets.h"

static const char *TAG = "UI";

/* 颜色 — RLCD 1-bit, 只有黑白 */
#define C_BLACK  lv_color_black()
#define C_WHITE  lv_color_white()

/* 字体 (与二/三屏共享) */
LV_FONT_DECLARE(custom_font_16_big);
LV_FONT_DECLARE(custom_font_14_big);
LV_FONT_DECLARE(font_num_28);
#define FONT_CN   (&custom_font_16_big)      /* 中文 (标题/主文本) */
#define FONT_CN_S (&custom_font_14_big)      /* 中文小字 (低/高/净值日, 含全 GB2312) */
#define FONT_NUM  (&font_num_28)             /* 大数字 (金价/温度) */

/* ===== 控件句柄 ===== */
/* 状态栏 */
static lv_obj_t *l_time, *l_batt, *l_wifi_txt;
static lv_obj_t *batt_box, *batt_fill, *sig_bar[4];
/* 天气区 */
static lv_obj_t *l_loc, *cur_ico, *l_cur_temp, *l_hilo;
static lv_obj_t *l_in_temp, *l_in_hum;
static int  cur_last_code = -1;  /* 天气图标仅在 code 变化时换, 避免反射屏无谓重绘 */
/* 黄金卡 */
static lv_obj_t *l_gold_price, *l_gold_hilo;
/* 基金表 */
static lv_obj_t *fund_labels[MAX_FUNDS][3];  /* [i][0]=name [1]=nav [2]=chg */
static lv_obj_t *fund_nav_hdr = NULL;
static int       fund_count = 0;
/* DeepSeek 卡 */
static lv_obj_t *l_ds_balance, *l_ds_today;

/* ===== 布局常量 (实机像素) — 压缩天气/黄金, 给基金腾行距 ===== */
#define STATUS_H   24
/* 天气区: y26~102 (高76) */
#define WX_Y       26
#define WX_H       76
#define WX_BIG_W   238          /* 左大卡宽 */
/* 黄金卡: y105~153 (高48, 调小) */
#define GD_Y       105
#define GD_H       48
/* 基金表: y156~272 (高116, 加高) */
#define FD_Y       156
#define FD_H       116
/* DeepSeek 卡: y275~297 (高22) */
#define DS_Y       275
#define DS_H       22

/* ===== ① 状态栏 ===== */
static void create_status_bar(lv_obj_t *scr)
{
    /* 电量文字 — 14 中文, 避免与电池框重叠 */
    l_batt = ui_mk(scr, "电量 --%", 6, 5, FONT_CN_S);

    /* 电池框 + 填充 + 头 (跟在文字后, "电量 100%"@14 约 66px) */
    batt_box = lv_obj_create(scr);
    lv_obj_set_size(batt_box, 26, 13);
    lv_obj_set_pos(batt_box, 74, 5);
    lv_obj_set_style_radius(batt_box, 2, 0);
    lv_obj_set_style_bg_opa(batt_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(batt_box, C_BLACK, 0);
    lv_obj_set_style_border_width(batt_box, 2, 0);
    lv_obj_set_style_pad_all(batt_box, 0, 0);
    lv_obj_set_scrollbar_mode(batt_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(batt_box, LV_OBJ_FLAG_SCROLLABLE);

    batt_fill = lv_obj_create(scr);
    lv_obj_set_size(batt_fill, 20, 7);
    lv_obj_set_pos(batt_fill, 77, 8);
    lv_obj_set_style_radius(batt_fill, 0, 0);
    lv_obj_set_style_bg_color(batt_fill, C_BLACK, 0);
    lv_obj_set_style_border_width(batt_fill, 0, 0);
    lv_obj_set_style_pad_all(batt_fill, 0, 0);
    lv_obj_set_scrollbar_mode(batt_fill, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(batt_fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tip = lv_obj_create(scr);
    lv_obj_set_size(tip, 3, 5);
    lv_obj_set_pos(tip, 100, 9);
    lv_obj_set_style_radius(tip, 0, 0);
    lv_obj_set_style_bg_color(tip, C_BLACK, 0);
    lv_obj_set_style_border_width(tip, 0, 0);
    lv_obj_set_style_pad_all(tip, 0, 0);
    lv_obj_set_scrollbar_mode(tip, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);

    /* 时间 — 居中 */
    l_time = lv_label_create(scr);
    lv_label_set_text(l_time, "--:--");
    lv_obj_set_style_text_color(l_time, C_BLACK, 0);
    lv_obj_set_style_text_font(l_time, &lv_font_montserrat_16, 0);
    lv_obj_align(l_time, LV_ALIGN_TOP_MID, 0, 4);

    /* WiFi 文字 (信号格左侧) */
    l_wifi_txt = lv_label_create(scr);
    lv_label_set_text(l_wifi_txt, "WiFi --");
    lv_obj_set_style_text_color(l_wifi_txt, C_BLACK, 0);
    lv_obj_set_style_text_font(l_wifi_txt, FONT_CN, 0);
    lv_obj_align(l_wifi_txt, LV_ALIGN_TOP_RIGHT, -39, 5);

    /* WiFi 信号格 (底对齐阶梯) */
    int base_y = 20;   /* 底基线 */
    for (int i = 0; i < 4; i++) {
        int h = 5 + i * 4;   /* 5/9/13/17 */
        lv_obj_t *s = lv_obj_create(scr);
        lv_obj_set_size(s, 4, h);
        lv_obj_set_pos(s, 375 + i * 5, base_y - h);
        lv_obj_set_style_radius(s, 0, 0);
        lv_obj_set_style_pad_all(s, 0, 0);
        lv_obj_set_style_border_color(s, C_BLACK, 0);
        lv_obj_set_style_border_width(s, 1, 0);
        lv_obj_set_style_bg_color(s, C_BLACK, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        sig_bar[i] = s;
    }

    /* 状态栏底分隔线 */
    ui_hline(scr, 5, STATUS_H, 400 - 10, 2);
}

/* ===== ② 天气区 ===== */
static void create_weather_area(lv_obj_t *scr)
{
    /* 左大卡 */
    lv_obj_t *big = ui_card(scr, 5, WX_Y, WX_BIG_W, WX_H);
    /* 城市/天气 竖排 (南京\n阴), 左侧垂直居中, 加行距让两字分开 */
    l_loc = ui_mk(big, WEATHER_CITY, 6, 0, FONT_CN);
    lv_obj_set_style_text_line_space(l_loc, 8, 0);
    lv_obj_align(l_loc, LV_ALIGN_LEFT_MID, 6, 0);

    /* 天气图标 (默认多云, 更新时按 code 替换) — 城市文字之后, 再右移一格 */
    cur_ico = lv_image_create(big);
    lv_image_set_src(cur_ico, ui_wmo_icon(3, true));
    lv_obj_align(cur_ico, LV_ALIGN_LEFT_MID, 56, -4);

    /* 大温度 (主视觉 28 大数字) — 城市/图标/温度 三段等间距 (~12px) */
    l_cur_temp = ui_mk(big, "--.-°C", 140, 18, FONT_NUM);
    /* 高低温 (次要 14) */
    l_hilo = ui_mk(big, "--~--°C", 144, 52, FONT_CN_S);

    /* 右上小卡 = 室内温度 (标签左 / 数值右, 同一中线并排) */
    int rx = 5 + WX_BIG_W + 4;                 /* =247 */
    int rw = 400 - rx - 5;                      /* =148 */
    int rh = (WX_H - 4) / 2;                     /* 每卡高 36 */
    lv_obj_t *c_temp = ui_card(scr, rx, WX_Y, rw, rh);
    /* 标签左、数值右, 同一条中线并排 (填掉右侧留白) */
    lv_obj_t *lt = ui_mk(c_temp, "室内温度", 8, 0, FONT_CN_S);
    lv_obj_align(lt, LV_ALIGN_LEFT_MID, 8, 0);
    l_in_temp = ui_mk(c_temp, "--.-°C", 0, 0, FONT_CN);
    lv_obj_align(l_in_temp, LV_ALIGN_RIGHT_MID, -8, 0);

    /* 右下小卡 = 湿度 */
    lv_obj_t *c_hum = ui_card(scr, rx, WX_Y + rh + 4, rw, rh);
    lv_obj_t *lh = ui_mk(c_hum, "湿 度", 8, 0, FONT_CN_S);
    lv_obj_align(lh, LV_ALIGN_LEFT_MID, 8, 0);
    l_in_hum = ui_mk(c_hum, "--%RH", 0, 0, FONT_CN);
    lv_obj_align(l_in_hum, LV_ALIGN_RIGHT_MID, -8, 0);
}

/* ===== ③ 黄金卡 (紧凑单行: 标题 | 价格 元/克 | 低/高) ===== */
static void create_gold_card(lv_obj_t *scr)
{
    lv_obj_t *c = ui_card(scr, 5, GD_Y, 400 - 10, GD_H);
    /* 全部改用 LEFT_MID 垂直居中 — 大价格(28)与小字(14)真正落在同一条中线上 */
    lv_obj_t *t = ui_mk(c, "黄金 (AU)", 8, 0, FONT_CN_S);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 0);

    /* 左组 标题/价格/元克 均匀铺开 */
    l_gold_price = ui_mk(c, "---.--", 0, 0, &lv_font_montserrat_16);
    lv_obj_align(l_gold_price, LV_ALIGN_LEFT_MID, 94, 0);

    lv_obj_t *u = ui_mk(c, "元/克", 0, 0, FONT_CN_S);
    lv_obj_align(u, LV_ALIGN_LEFT_MID, 156, 0);

    /* 低/高 — 右对齐, 距卡右边固定 12px */
    l_gold_hilo = ui_mk(c, "低---  高---", 0, 0, FONT_CN_S);
    lv_obj_align(l_gold_hilo, LV_ALIGN_RIGHT_MID, -12, 0);
}

/* ===== ④ 基金表 ===== */
static void create_fund_table(lv_obj_t *scr)
{
    lv_obj_t *c = ui_card(scr, 5, FD_Y, 400 - 10, FD_H);

    /* 表头 (统一 14, 与数据行一致更协调) */
    ui_mk(c, "基金名称", 8, 5, FONT_CN_S);
    fund_nav_hdr = ui_mk(c, "", 150, 6, FONT_CN_S);   /* 净值日期 — 中文字体避免方框 */
    ui_mk(c, "净值", 262, 5, FONT_CN_S);
    lv_obj_t *h_chg = ui_mk(c, "涨跌幅", 0, 5, FONT_CN_S);
    lv_obj_set_width(h_chg, 60);
    lv_obj_set_style_text_align(h_chg, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(h_chg, LV_ALIGN_TOP_RIGHT, -8, 5);

    /* 表头下分隔线 */
    ui_hline(c, 6, 24, 400 - 10 - 12, 2);

    /* 三行 (表头下 y30, 步距 28: 卡高116 拉开行距, 不再挤) */
    for (int i = 0; i < MAX_FUNDS; i++) {
        int ry = 32 + i * 28;

        fund_labels[i][0] = ui_mk(c, "-", 8, ry, FONT_CN_S);   /* 名称 */
        fund_labels[i][1] = ui_mk(c, "-", 262, ry, FONT_CN_S); /* 净值 */

        fund_labels[i][2] = ui_mk(c, "-", 0, ry, FONT_CN_S);   /* 涨跌幅 右对齐 */
        lv_obj_set_width(fund_labels[i][2], 70);
        lv_obj_set_style_text_align(fund_labels[i][2], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(fund_labels[i][2], LV_ALIGN_TOP_RIGHT, -8, ry);
    }
    fund_count = MAX_FUNDS;
}

/* ===== ⑤ DeepSeek 卡 ===== */
static void create_ds_card(lv_obj_t *scr)
{
    lv_obj_t *c = ui_card(scr, 5, DS_Y, 400 - 10, DS_H);

    /* 名称 — 左侧垂直居中 */
    lv_obj_t *nm = ui_mk(c, "DeepSeek", 8, 0, FONT_CN_S);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, 0);

    /* 今日用量 — 中间 (仅 Bridge 模式有 token/费用; 直连只有余额时留空) */
    l_ds_today = ui_mk(c, "", 0, 0, FONT_CN_S);
    lv_obj_align(l_ds_today, LV_ALIGN_CENTER, 10, 0);

    /* 余额 — 右侧垂直居中, 用"元"避免 ¥ 缺字方框 */
    l_ds_balance = ui_mk(c, "余额 --.-- 元", 0, 0, FONT_CN_S);
    lv_obj_align(l_ds_balance, LV_ALIGN_RIGHT_MID, -12, 0);
}

/* ===== 初始化 ===== */
void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    create_status_bar(scr);
    create_weather_area(scr);
    create_gold_card(scr);
    create_fund_table(scr);
    create_ds_card(scr);
    ESP_LOGI(TAG, "UI initialized (card layout)");
}

/* ===== 更新所有 UI ===== */
void ui_update_all(const AppData_t *app)
{
    char buf[80];

    /* ── 状态栏: 时间 ── */
    get_time_str(buf);
    ui_set_label(l_time, buf);

    /* 电池文字 + 填充宽度 */
    if (l_batt) {
        /* 始终显示实时 ADC 电量 (充电时也随电压缓慢变化, 不再冻结成插线快照).
         * 充电中/满的精确判定需硬件 STAT 脚, 本板未引到 GPIO, 只能看板载绿灯 CHG. */
        int pct = app->battery_pct;
        bool charging = (app->bat_drop_per_h < 0);
        snprintf(buf, sizeof(buf), charging ? "充电 %d%%" : "电量 %d%%", pct);
        ui_set_label(l_batt, buf);
        if (batt_fill) {
            int w = 20 * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
            if (w < 1) w = 1;
            lv_obj_set_width(batt_fill, w);
        }
    }

    /* WiFi 文字 + 信号格 */
    bool night = wifi_is_night_sleep();
    bool conn  = wifi_is_connected();
    ui_set_label(l_wifi_txt, night ? "WiFi Zzz" : (conn ? "WiFi OK" : "WiFi NO"));
    /* 信号格: 连上点亮全部, 未连全灭 (无 RSSI 分级数据时的近似) */
    ui_sig_set(sig_bar, 4, (conn && !night) ? 4 : 0);

    /* ── 天气区 ── */
    /* 城市/天气状况 — 竖排两行 (南京\n阴) */
    if (app->weather.condition[0] != '\0')
        snprintf(buf, sizeof(buf), "%s\n%s", WEATHER_CITY, app->weather.condition);
    else
        snprintf(buf, sizeof(buf), "%s", WEATHER_CITY);
    ui_set_label(l_loc, buf);

    /* 天气图标 (仅 code 变化时换, 避免反射屏无谓重绘) */
    if (app->weather.code != cur_last_code) {
        lv_image_set_src(cur_ico, ui_wmo_icon(app->weather.code, true));
        cur_last_code = app->weather.code;
    }

    /* 大温度 = 室外当前温度 */
    if (app->weather.condition[0] != '\0') {
        snprintf(buf, sizeof(buf), "%.1f°C", app->weather.temp_outdoor);
        ui_set_label(l_cur_temp, buf);
        snprintf(buf, sizeof(buf), "%.0f~%.0f°C",
                 app->weather.temp_min, app->weather.temp_max);
        ui_set_label(l_hilo, buf);
    } else {
        ui_set_label(l_cur_temp, "--.-°C");
        ui_set_label(l_hilo, "--~--°C");
    }

    /* 室内温度 / 湿度 */
    snprintf(buf, sizeof(buf), "%.1f°C", app->indoor_temp);
    ui_set_label(l_in_temp, buf);
    snprintf(buf, sizeof(buf), "%d%%RH", (int)app->indoor_hum);
    ui_set_label(l_in_hum, buf);

    /* ── 黄金卡 ── */
    if (app->gold.price > 1) {
        snprintf(buf, sizeof(buf), "%.2f", app->gold.price);
        ui_set_label(l_gold_price, buf);
        if (app->gold.high > 1 || app->gold.low > 1) {
            snprintf(buf, sizeof(buf), "低 %.2f   高 %.2f",
                     app->gold.low, app->gold.high);
        } else {
            snprintf(buf, sizeof(buf), "低 ---   高 ---");
        }
        ui_set_label(l_gold_hilo, buf);
    } else {
        ui_set_label(l_gold_price, "---.--");
        ui_set_label(l_gold_hilo, "等待联网...");
    }

    /* ── 基金表 ── */
    for (int i = 0; i < fund_count; i++) {
        const FundItem_t *f = &app->funds[i];
        if (i < app->fund_count && f->nav > 0) {
            char name[80];
            const char *src = (f->name[0] != '\0') ? f->name : api_fund_default_name(i);
            strncpy(name, src, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            char *rmb = strstr(name, "人民币A");
            if (rmb) *rmb = '\0';
            ui_set_label(fund_labels[i][0], name);

            snprintf(buf, sizeof(buf), "%.4f", f->nav);
            ui_set_label(fund_labels[i][1], buf);

            snprintf(buf, sizeof(buf), "%s%.2f%%",
                     f->is_up ? "+" : "-", f->change_pct < 0 ? -f->change_pct : f->change_pct);
            ui_set_label(fund_labels[i][2], buf);
        } else {
            ui_set_label(fund_labels[i][0], api_fund_default_name(i));
            ui_set_label(fund_labels[i][1], "--");
            ui_set_label(fund_labels[i][2], "--");
        }
    }

    /* 净值日期 */
    if (fund_nav_hdr) {
        const char *d = "";
        for (int i = 0; i < app->fund_count && i < MAX_FUNDS; i++)
            if (app->funds[i].nav_date[0]) { d = app->funds[i].nav_date; break; }
        if (d[0]) {
            snprintf(buf, sizeof(buf), "净值日 %s", d);
            ui_set_label(fund_nav_hdr, buf);
        }
    }

    /* ── DeepSeek 卡 ── */
    if (app->ds.balance > 0) {
        snprintf(buf, sizeof(buf), "余额 %.2f 元", app->ds.balance);
        ui_set_label(l_ds_balance, buf);
    }
    /* 今日 token/费用 (仅 Bridge 模式返回; 直连留空) */
    if (app->ds.today_tokens_m > 0 || app->ds.today_cost > 0) {
        snprintf(buf, sizeof(buf), "今 %.2fM  %.2f元", app->ds.today_tokens_m, app->ds.today_cost);
        ui_set_label(l_ds_today, buf);
    }
}

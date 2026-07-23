/**
 * @file ui_main.cpp
 * @brief LVGL 主界面 — 基金监控/天气/黄金/DeepSeek
 *
 * 布局 (400x300):
 * ┌────────────────────────────────────────────┐
 * │ ◆ RLCD MONITOR    15:02:38    WiFi 06/22  │  ← 状态栏 24px
 * ├────────────────────────────────────────────┤
 * │ ☁ 22~30°C 上海    室内26.3°C 65%RH PM35   │  ← 天气行 28px
 * ├────────────────────────────────────────────┤
 * │ Au ¥568.20 ▲+2.15    高572.30 低566.10    │  ← 黄金行 24px
 * ├───────────┬────────────────────────────────┤
 * │ 总资产    │ 基金      净值   涨跌   走势     │
 * │           │                                 │
 * │ ¥86,432   │ 天弘中证... 1.2456 ▲+0.89% ██  │  ← 主内容区
 * │ +1,246 ▲  │ ...                            │  ← 可滚动
 * │           │                                 │
 * │ 涨3 跌2   │                                 │
 * ├───────────┴────────────────────────────────┤
 * │ 🐋 ¥37.42  今日161M ¥5.64  本月230M ¥8.08  │  ← DeepSeek 32px
 * ├────────────────────────────────────────────┤
 * │ 更新15:02  ◉交易中   天天基金·上金所·DS    │  ← 底部 16px
 * └────────────────────────────────────────────┘
 */

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <lvgl.h>
#include "ui_main.h"
#include "wifi_app.h"
#include "secrets.h"

/* Custom SimHei font for Chinese characters (1bpp) */
LV_FONT_DECLARE(custom_font_14_big);
#define FONT_CN  (&custom_font_14_big)
#define FONT_CN_BIG (&custom_font_14_big)

static const char *TAG = "UI";

/* LVGL 颜色 — RLCD 1-bit, 只有黑白 */
#define C_BLACK  lv_color_black()
#define C_WHITE  lv_color_white()

/* 仅在文本变化时才 set: lv_label_set_text 不比较内容, 每次都 invalidate 触发整屏重绘.
 * 用 LVGL 自存的当前文本做比较, 内容不变则跳过, 屏幕静止时零重绘 (省电/防闪). */
static void set_label(lv_obj_t *l, const char *txt)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(l, txt);
}

/* 布局常量 */
#define SCREEN_W     400
#define SCREEN_H     300
#define MARGIN       4
#define STATUS_H     26
#define WEATHER_H    26
#define GOLD_H       26
#define DS_H         72
#define FOOTER_H     0
#define CONTENT_Y    (STATUS_H + WEATHER_H + GOLD_H)
#define CONTENT_H    (SCREEN_H - CONTENT_Y - DS_H)

/* UI 控件句柄 */
static lv_obj_t *ui_status_time;
static lv_obj_t *ui_status_date;
static lv_obj_t *ui_battery;
static lv_obj_t *ui_weather_text;
static lv_obj_t *ui_weather_indoor;
static lv_obj_t *ui_gold_text;
static lv_obj_t *ui_gold_change;
static lv_obj_t *fund_labels[MAX_FUNDS][3]; /* [i][0]=name, [1]=nav, [2]=chg */
static lv_obj_t *fund_nav_hdr = NULL;       /* "净值"表头, 附带净值日期 */
/* 走势柱条已移除 */
static int       fund_count = 0;
static const char *fund_names[MAX_FUNDS] = {"摩根日本精选股票(QDII)A","摩根纳斯达克100指数(QDII)","广发全球精选股票(QDII)"};
static lv_obj_t *ui_ds_balance;
/* footer removed */

/* ===== 状态栏 ===== */
static void create_status_bar(lv_obj_t *parent)
{
    /* 电池电量 */
    ui_battery = lv_label_create(parent);
    lv_label_set_text(ui_battery, "电量 --%");
    lv_obj_set_style_text_color(ui_battery, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_battery, &*FONT_CN, 0);
    lv_obj_align(ui_battery, LV_ALIGN_TOP_LEFT, 2, 2);

    /* 时间 — 居中 */
    ui_status_time = lv_label_create(parent);
    lv_label_set_text(ui_status_time, "--:--:--");
    lv_obj_set_style_text_color(ui_status_time, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_status_time, &lv_font_montserrat_14, 0);
    lv_obj_align(ui_status_time, LV_ALIGN_TOP_MID, 0, 2);

    /* 日期 + WiFi */
    ui_status_date = lv_label_create(parent);
    lv_label_set_text(ui_status_date, "W:-- --/--");
    lv_obj_set_style_text_color(ui_status_date, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_status_date, &*FONT_CN, 0);
    lv_obj_align(ui_status_date, LV_ALIGN_TOP_RIGHT, -2, 3);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCREEN_W - MARGIN * 2, 1);
    lv_obj_set_style_bg_color(line, C_BLACK, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 2, STATUS_H - 2);
}

/* ===== 天气行 ===== */
static void create_weather_row(lv_obj_t *parent)
{
    int y = STATUS_H + 2;

    /* 天气图标+温度 — 使用中文字体以支持 °C 等字符回退 */
    ui_weather_text = lv_label_create(parent);
    lv_label_set_text(ui_weather_text, "---.--°C");  /* 天气由更新函数填充 */
    lv_obj_set_style_text_color(ui_weather_text, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_weather_text, &*FONT_CN, 0);
    lv_obj_align(ui_weather_text, LV_ALIGN_TOP_LEFT, 2, y);

    /* 室内温湿度 */
    ui_weather_indoor = lv_label_create(parent);
    lv_label_set_text(ui_weather_indoor, "室内--°C");  /* 室内--°C */
    lv_obj_set_style_text_color(ui_weather_indoor, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_weather_indoor, &*FONT_CN, 0);
    lv_obj_align(ui_weather_indoor, LV_ALIGN_TOP_RIGHT, -2, y);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCREEN_W - MARGIN * 2, 1);
    lv_obj_set_style_bg_color(line, C_BLACK, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 2, STATUS_H + WEATHER_H + 2);
}

/* ===== 贵金属行: 黄金 + 白银 ===== */
static void create_gold_row(lv_obj_t *parent)
{
    int y = STATUS_H + WEATHER_H + 5;

    /* 黄金 */
    ui_gold_text = lv_label_create(parent);
    lv_label_set_text(ui_gold_text, "黄金 ---.-- 元/克");
    lv_obj_set_style_text_color(ui_gold_text, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_gold_text, &*FONT_CN, 0);
    lv_obj_align(ui_gold_text, LV_ALIGN_TOP_LEFT, 2, y);

    ui_gold_change = lv_label_create(parent);
    lv_label_set_text(ui_gold_change, "---.--  高--- 低---");
    lv_obj_set_style_text_color(ui_gold_change, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_gold_change, &*FONT_CN, 0);
    lv_obj_align(ui_gold_change, LV_ALIGN_TOP_LEFT, 240, y + 2);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, SCREEN_W - MARGIN * 2, 1);
    lv_obj_set_style_bg_color(line, C_BLACK, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 2, STATUS_H + WEATHER_H + GOLD_H);
}

/* ===== 白银 (在黄金下方) ===== */
/* 白银已移除 */

/* ===== 主内容区: 左侧资产 + 右侧基金 ===== */
static void create_content_area(lv_obj_t *parent)
{
    int cy = CONTENT_Y;
    int rx = 4;   /* 基金列表起始X */
    int fy = cy;

    /* ---- 基金列表 (全宽, 左侧总资产已移除) ---- */

    /* 表头 */
    lv_obj_t *h_name  = lv_label_create(parent);
    lv_label_set_text(h_name, "基金");
    lv_obj_set_style_text_color(h_name, C_BLACK, 0);
    lv_obj_set_style_text_font(h_name, &*FONT_CN, 0);
    lv_obj_align(h_name, LV_ALIGN_TOP_LEFT, rx, fy);

    lv_obj_t *h_nav = lv_label_create(parent);
    lv_label_set_text(h_nav, "净值");
    lv_obj_set_style_text_color(h_nav, C_BLACK, 0);
    lv_obj_set_style_text_font(h_nav, &*FONT_CN, 0);
    lv_obj_align(h_nav, LV_ALIGN_TOP_LEFT, rx + 220, fy);

    /* 净值日期: 放在"基金"表头右侧的空白区, 自解释, 不挤净值/涨跌列 */
    fund_nav_hdr = lv_label_create(parent);
    lv_label_set_text(fund_nav_hdr, "");
    lv_obj_set_style_text_color(fund_nav_hdr, C_BLACK, 0);
    lv_obj_set_style_text_font(fund_nav_hdr, &*FONT_CN, 0);
    lv_obj_align(fund_nav_hdr, LV_ALIGN_TOP_LEFT, rx + 70, fy);

    lv_obj_t *h_chg = lv_label_create(parent);
    lv_label_set_text(h_chg, "涨跌");
    lv_obj_set_style_text_color(h_chg, C_BLACK, 0);
    lv_obj_set_style_text_font(h_chg, &*FONT_CN, 0);
    lv_obj_align(h_chg, LV_ALIGN_TOP_LEFT, rx + 310, fy);

    /* 基金行 */
    for (int i = 0; i < MAX_FUNDS; i++) {
        int row_y = fy + 24 + i * 35;
        if (row_y + 22 > cy + CONTENT_H) break;

        fund_labels[i][0] = lv_label_create(parent);  /* 名称 */
        lv_label_set_text(fund_labels[i][0], "-");
        lv_obj_set_style_text_color(fund_labels[i][0], C_BLACK, 0);
        lv_obj_set_style_text_font(fund_labels[i][0], &*FONT_CN, 0);
        lv_obj_align(fund_labels[i][0], LV_ALIGN_TOP_LEFT, rx, row_y);

        fund_labels[i][1] = lv_label_create(parent);  /* 净值 */
        lv_label_set_text(fund_labels[i][1], "-");
        lv_obj_set_style_text_color(fund_labels[i][1], C_BLACK, 0);
        lv_obj_set_style_text_font(fund_labels[i][1], &*FONT_CN, 0);
        lv_obj_align(fund_labels[i][1], LV_ALIGN_TOP_LEFT, rx + 220, row_y);

        fund_labels[i][2] = lv_label_create(parent);  /* 涨跌 */
        lv_label_set_text(fund_labels[i][2], "-");
        lv_obj_set_style_text_color(fund_labels[i][2], C_BLACK, 0);
        lv_obj_set_style_text_font(fund_labels[i][2], &*FONT_CN, 0);
        lv_obj_align(fund_labels[i][2], LV_ALIGN_TOP_LEFT, rx + 310, row_y);
    }

    fund_count = MAX_FUNDS;
}

/* ===== DeepSeek 区域 ===== */
static void create_ds_section(lv_obj_t *parent)
{
    int y = SCREEN_H - DS_H;  /* 底部向上 */

    /* 分隔线 */
    lv_obj_t *div = lv_obj_create(parent);
    lv_obj_set_size(div, SCREEN_W - 8, 1);
    lv_obj_set_style_bg_color(div, C_BLACK, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_align(div, LV_ALIGN_TOP_LEFT, 4, y);

    /* DeepSeek 标题 + 余额 (同行, 左标题右余额) */
    lv_obj_t *ds_t = lv_label_create(parent);
    lv_label_set_text(ds_t, "DeepSeek");
    lv_obj_set_style_text_color(ds_t, C_BLACK, 0);
    lv_obj_set_style_text_font(ds_t, &lv_font_montserrat_12, 0);
    lv_obj_align(ds_t, LV_ALIGN_TOP_LEFT, 4, y + 1);

    ui_ds_balance = lv_label_create(parent);
    lv_label_set_text(ui_ds_balance, "余额 --.--");
    lv_obj_set_style_text_color(ui_ds_balance, C_BLACK, 0);
    lv_obj_set_style_text_font(ui_ds_balance, &*FONT_CN, 0);
    lv_obj_align(ui_ds_balance, LV_ALIGN_TOP_RIGHT, -4, y + 1);

}

/* ===== 初始化 ===== */
void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);

    /* 设置中文字体回退到 Montserrat，确保 °、°C 等西文符号 */
    ((lv_font_t *)(FONT_CN))->fallback = &lv_font_montserrat_14;
    ((lv_font_t *)(FONT_CN_BIG))->fallback = &lv_font_montserrat_14;

    create_status_bar(scr);
    create_weather_row(scr);
    create_gold_row(scr);
    /* silver removed */
    create_content_area(scr);
    create_ds_section(scr);
    ESP_LOGI(TAG, "UI initialized");
}

/* ===== 更新所有 UI ===== */
void ui_update_all(const AppData_t *app)
{
    char buf[64];

    /* 状态栏 */
    get_time_str(buf);
    set_label(ui_status_time, buf);

    /* 电池 */
    if (ui_battery) {
        if (app->bat_drop_per_h < 0) {
            snprintf(buf, sizeof(buf), "充电 %d%%", app->bat_charge_pct);
            set_label(ui_battery, buf);
        } else {
            snprintf(buf, sizeof(buf), "电量 %d%%", app->battery_pct);
            set_label(ui_battery, buf);
        }
    }

    /* WiFi 状态 + 日期 */
    char date_buf[32];
    time_t t_now;
    struct tm ti;
    time(&t_now);
    localtime_r(&t_now, &ti);
    const char *wday_cn[] = {"日","一","二","三","四","五","六"};
    /* 夜间省电时射频已关, 显示"WiFi:Zzz"(休眠)而非"WiFi:NO", 避免误以为故障.
     * 用 ASCII Zzz 而非中文"省电": 精简字库无"省"字, 会渲染成方框 */
    const char *wifi_str = wifi_is_night_sleep() ? "WiFi:Zzz"
                         : (wifi_is_connected() ? "WiFi:OK" : "WiFi:NO");
    snprintf(date_buf, sizeof(date_buf), "%s %02d/%02d %s%s",
             wifi_str,
             ti.tm_mon + 1, ti.tm_mday,
             "星期",
             wday_cn[ti.tm_wday]);
    set_label(ui_status_date, date_buf);

    /* 室外天气 — 显示城市南京 (有天气数据即显示, 不能用温度阈值判断, 否则冬天 ≤0.5°C 会被误判为无数据) */
    if (app->weather.condition[0] != '\0') {
        snprintf(buf, sizeof(buf), "南京.%s %.0f-%.0fC",
                 app->weather.condition,
                 app->weather.temp_min,
                 app->weather.temp_max);
    } else {
        snprintf(buf, sizeof(buf), "-- ~--°C");
    }
    set_label(ui_weather_text, buf);

    /* 室内温湿度 + PM2.5 */
    snprintf(buf, sizeof(buf), "室内%.1f°C 湿度%d%%RH PM%d",
             app->indoor_temp, (int)app->indoor_hum,
             app->weather.pm25 > 0 ? app->weather.pm25 : 35);
    set_label(ui_weather_indoor, buf);

    /* 黄金 */
    if (app->gold.price > 1) {
        snprintf(buf, sizeof(buf), "黄金 %.2f 元/克", app->gold.price);
        set_label(ui_gold_text, buf);
        if (app->gold.high > 1 || app->gold.low > 1) {
            snprintf(buf, sizeof(buf), "低%.2f 高%.2f", app->gold.low, app->gold.high);
        } else {
            snprintf(buf, sizeof(buf), "---");
        }
        set_label(ui_gold_change, buf);
    } else {
        set_label(ui_gold_text, "黄金 --- 元/克");
        set_label(ui_gold_change, "等待联网...");
    }

    /* 白银已移除 */
    /* 总资产已移除 */

    /* 基金列表 — 显示所有3个槽位 */
    for (int i = 0; i < fund_count; i++) {
        const FundItem_t *f = &app->funds[i];
        if (i < app->fund_count && f->nav > 0) {
            /* 有数据 — 正常显示 */
            char name[80];
            const char *src = (f->name[0] != '\0') ? f->name : fund_names[i];
            strncpy(name, src, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
            /* 去掉尾部的"人民币A"节省空间 */
            char *rmb = strstr(name, "人民币A");
            if (rmb) *rmb = '\0';
            set_label(fund_labels[i][0], name);

            snprintf(buf, sizeof(buf), "%.4f", f->nav);
            set_label(fund_labels[i][1], buf);

            snprintf(buf, sizeof(buf), "%s%.2f%%",
                     f->is_up ? "▲" : "▼", f->change_pct);
            set_label(fund_labels[i][2], buf);
        } else {
            set_label(fund_labels[i][0], fund_names[i] ? fund_names[i] : "-");
            set_label(fund_labels[i][1], "--");
            set_label(fund_labels[i][2], "--");
        }
    }  /* for fund_count */

    /* 净值日期 (QDII 为 T-1/T-2): 显示在"基金"右侧空白区, 自解释 */
    if (fund_nav_hdr) {
        const char *d = "";
        for (int i = 0; i < app->fund_count && i < MAX_FUNDS; i++)
            if (app->funds[i].nav_date[0]) { d = app->funds[i].nav_date; break; }
        if (d[0]) {
            snprintf(buf, sizeof(buf), "净值日 %s", d);
            set_label(fund_nav_hdr, buf);
        }
    }

    /* DeepSeek */
    if (app->ds.balance > 0) {
        snprintf(buf, sizeof(buf), "余额 %.2f", app->ds.balance);
        lv_label_set_text(ui_ds_balance, buf);
    }

}

/**
 * @file ui_sysinfo.cpp — 第三屏: 系统监控 (2×2 卡片 HUD 风, 仿 cpu.png)
 *
 * 硬件: 4.2" 全反射 1-bit 单色屏, 原生 300×400 (本项目横用 400×300), 无背光.
 * 据此的设计约束:
 *   ① 只有纯黑/纯白, 无灰阶 —— 不用半透明淡色, 层次靠字号区分;
 *   ② 细线在反射屏会糊 —— 边框/分隔线 ≥2px, 无 1px 虚线;
 *   ③ 静止显示最省电 —— set_label 只在变化时刷新.
 *
 * 布局 (400×300, 白底黑字):
 * ┌─ 标题栏  ← ESP32-S3 系统监控 ───────────── 28px
 * ├─ 信息行  IP / NTP / UP ─────────────────── 20px
 * ├─ 2×2 卡片 (完整方框 + 四角 HUD 括号) ────── 剩余
 * │  内存(大数字+分段条)   任务数(纯大数字)
 * │  CPU(大数字+分段条)    WiFi(信号格+文本)
 * └───────────────────────────────────────────
 *
 * 大数字: font_hud_28 (含字母 K/M/%). 中文标题: custom_font_16_big.
 * 通用控件 (ui_mk/ui_card/ui_hline/ui_set_label/ui_sig_set) 来自 ui_common.
 * 分段条/信号格是本屏特有几何, 保留本地绘制.
 */

#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "ui_sysinfo.h"
#include "ui_common.h"
#include "wifi_app.h"
#include "api_clients.h"

static const char *TAG = "SYS";

#define C_BLACK lv_color_black()
#define C_WHITE lv_color_white()

LV_FONT_DECLARE(custom_font_16_big);
LV_FONT_DECLARE(font_hud_28);
#define FONT_CN   (&custom_font_16_big)          /* 中文标题 */
#define FONT_S    (&lv_font_montserrat_12)       /* 小字/英文标签 */
#define FONT_M    (&lv_font_montserrat_14)       /* 卡片内文本 */
#define FONT_BIG  (&font_hud_28)                 /* 大数字 (数字+K/M/H/z 专用) */

/* 卡片网格几何 (400×300 实际像素) */
#define CARD_TOP   54                              /* 卡片区起始 y */
#define CARD_GAP   6
#define CARD_MX    6
#define CARD_W     ((400 - CARD_MX*2 - CARD_GAP) / 2)    /* =191 */
#define CARD_H     ((300 - CARD_TOP - CARD_GAP - 2) / 2) /* =119 */

/* 分段方块条: 10 格 */
#define SEG_N      10
#define SEG_H      12

static lv_obj_t *scr;
static lv_obj_t *l_info;                 /* 信息行 IP/NTP/UP */
static lv_obj_t *l_mem_big, *l_mem_pct;  /* 内存: 剩余K大数字 + USED% 标注 */
static lv_obj_t *mem_seg[SEG_N];
static lv_obj_t *l_task_big;             /* 任务数大数字 */
static lv_obj_t *l_cpu_big, *l_cpu_sub;  /* CPU 频率大数字 + Flash/PSRAM */
static lv_obj_t *cpu_seg[SEG_N];
static lv_obj_t *sig_bar[4];             /* WiFi 信号格 */
static lv_obj_t *l_wifi_ssid, *l_wifi_sub;

/* 分段方块条: 一圈外框轨道 + SEG_N 个实心块.
 * 轨道始终可见 (单层 2px 外框) 表示量程, 低占用时也有完整仪表轮廓, 不再是孤块.
 * 点亮块为实心, 空格透明 —— 框内看到的是"填了几格"而非一排碎边. */
static void seg_bar(lv_obj_t *p, lv_coord_t x, lv_coord_t y, lv_obj_t **out)
{
    int usable = CARD_W - 20;                 /* 左右各留 10 */
    int gap = 3;
    int sw = (usable - (SEG_N - 1) * gap) / SEG_N;
    int span = (SEG_N - 1) * (sw + gap) + sw; /* 所有块横向总跨度 */

    /* 轨道: 包住全部块, 四周留 2px 内边距, 单层外框 */
    lv_obj_t *tr = lv_obj_create(p);
    lv_obj_set_size(tr, span + 6, SEG_H + 6);
    lv_obj_set_pos(tr, x - 3, y - 3);
    lv_obj_set_style_radius(tr, 2, 0);
    lv_obj_set_style_pad_all(tr, 0, 0);
    lv_obj_set_style_bg_opa(tr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(tr, C_BLACK, 0);
    lv_obj_set_style_border_width(tr, 2, 0);
    lv_obj_set_scrollbar_mode(tr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tr, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < SEG_N; i++) {
        lv_obj_t *s = lv_obj_create(p);
        lv_obj_set_size(s, sw, SEG_H);
        lv_obj_set_pos(s, x + i * (sw + gap), y);
        lv_obj_set_style_radius(s, 0, 0);
        lv_obj_set_style_pad_all(s, 0, 0);
        lv_obj_set_style_border_width(s, 0, 0);          /* 块不描边: 靠轨道给边界 */
        lv_obj_set_style_bg_color(s, C_BLACK, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);   /* 默认不可见, update 点亮为实心块 */
        lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        out[i] = s;
    }
}

/* 点亮前 k 格 (实心), 其余空心 */
static void seg_set(lv_obj_t **seg, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int k = (pct * SEG_N + 50) / 100;      /* 四舍五入到格 */
    ui_sig_set(seg, SEG_N, k);
}

void sysinfo_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 标题栏 ── */
    ui_mk(scr, LV_SYMBOL_LEFT, 8, 7, FONT_M);
    ui_mk(scr, "ESP32-S3 系统监控", 34, 4, FONT_CN);
    ui_hline(scr, 6, 30, 388, 2);   /* 粗分隔线 */

    /* ── 信息行 ── */
    l_info = ui_mk(scr, "IP ---.---.---.---  NTP --:--  UP --:--:--", 8, 36, FONT_S);

    /* ── 2×2 卡片 ── */
    int x0 = CARD_MX, x1 = CARD_MX + CARD_W + CARD_GAP;
    int y0 = CARD_TOP, y1 = CARD_TOP + CARD_H + CARD_GAP;

    /* 卡片1: 内存 (左上) — 剩余K大数字 + 分段条 */
    lv_obj_t *c1 = ui_card(scr, x0, y0, CARD_W, CARD_H);
    ui_mk(c1, "内存", 10, 6, FONT_CN);
    ui_mk(c1, "MEM", CARD_W - 42, 9, FONT_S);
    l_mem_big = ui_mk(c1, "----K", 10, 28, FONT_BIG);
    ui_mk(c1, "/ 6144K", 12, 60, FONT_M);
    l_mem_pct = ui_mk(c1, "USED --%", CARD_W - 78, 60, FONT_S);
    seg_bar(c1, 10, 86, mem_seg);

    /* 卡片2: 任务数 (右上) — 纯大数字 */
    lv_obj_t *c2 = ui_card(scr, x1, y0, CARD_W, CARD_H);
    ui_mk(c2, "任务数", 10, 6, FONT_CN);
    ui_mk(c2, "TASKS", CARD_W - 52, 9, FONT_S);
    l_task_big = ui_mk(c2, "--", 16, 34, FONT_BIG);
    ui_mk(c2, "RUNNING", 16, 74, FONT_M);
    ui_mk(c2, "MAX 30", CARD_W - 66, 74, FONT_M);

    /* 卡片3: CPU (左下) — 频率大数字 + 分段条 */
    lv_obj_t *c3 = ui_card(scr, x0, y1, CARD_W, CARD_H);
    ui_mk(c3, "CPU", 10, 6, FONT_CN);
    ui_mk(c3, "CLK", CARD_W - 40, 9, FONT_S);
    l_cpu_big = ui_mk(c3, "---MHz", 10, 28, FONT_BIG);
    seg_bar(c3, 10, 64, cpu_seg);
    l_cpu_sub = ui_mk(c3, "Flash 16M  PSRAM --M", 10, 88, FONT_M);

    /* 卡片4: WiFi (右下) — 信号格 + SSID/dBm */
    lv_obj_t *c4 = ui_card(scr, x1, y1, CARD_W, CARD_H);
    ui_mk(c4, "WiFi", 10, 6, FONT_CN);
    ui_mk(c4, "LINK", CARD_W - 44, 9, FONT_S);
    /* 信号 4 格 (底对齐, 阶梯上升). base_y: 信号格+文字整块居中略偏上, 与 CPU 分段条齐平 */
    int sx = 18, base_y = 78;
    for (int i = 0; i < 4; i++) {
        int h = 10 + i * 8;   /* 10/18/26/34, span y[44,78] */
        lv_obj_t *s = lv_obj_create(c4);
        lv_obj_set_size(s, 12, h);
        lv_obj_set_pos(s, sx + i * 16, base_y - h);
        lv_obj_set_style_radius(s, 0, 0);
        lv_obj_set_style_pad_all(s, 0, 0);
        lv_obj_set_style_border_color(s, C_BLACK, 0);
        lv_obj_set_style_border_width(s, 2, 0);
        lv_obj_set_style_bg_color(s, C_BLACK, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
        sig_bar[i] = s;
    }
    /* SSID/dBm 放信号格右侧, 两行竖直居中对齐格子 (格子 y[44,78], 中心 ~61) */
    l_wifi_ssid = ui_mk(c4, "--------", 90, 46, FONT_M);
    l_wifi_sub  = ui_mk(c4, "--- dBm  ch--", 90, 68, FONT_S);

    ESP_LOGI(TAG, "sysinfo card dashboard created");
}

/* RSSI → 点亮几格 (0~4) */
static int rssi_level(int rssi, bool connected, bool night)
{
    if (night || !connected || rssi == 0) return 0;
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    return 1;
}

void sysinfo_update(const AppData_t *app)
{
    (void)app;
    char b[64];

    /* ── 信息行: IP / NTP / UP ── */
    uint64_t us = esp_timer_get_time();
    unsigned up_h = (unsigned)(us / 3600000000ULL);
    unsigned up_m = (unsigned)((us % 3600000000ULL) / 60000000ULL);
    unsigned up_s = (unsigned)((us % 60000000ULL) / 1000000ULL);
    time_t t; time(&t);
    struct tm ti; localtime_r(&t, &ti);
    char ntp[8];
    if (ti.tm_year >= (2024 - 1900))
        snprintf(ntp, sizeof ntp, "%02d:%02d", ti.tm_hour, ti.tm_min);
    else
        snprintf(ntp, sizeof ntp, "--:--");
    snprintf(b, sizeof b, "IP %s  NTP %s  UP %02u:%02u:%02u",
             wifi_get_ip(), ntp, up_h, up_m, up_s);
    ui_set_label(l_info, b);

    /* ── 卡片1: 内存 ── (总量固定, 缓存避免每次遍历堆区统计) */
    static size_t s_ram_total = 0;
    if (s_ram_total == 0)
        s_ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL)
                    + heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t r_total = s_ram_total;
    size_t r_free  = esp_get_free_heap_size();
    snprintf(b, sizeof b, "%uK", (unsigned)(r_free / 1024));
    ui_set_label(l_mem_big, b);
    int used_pct = (r_total > 0) ? (int)((r_total - r_free) * 100 / r_total) : 0;
    snprintf(b, sizeof b, "USED %d%%", used_pct);
    ui_set_label(l_mem_pct, b);
    seg_set(mem_seg, used_pct);

    /* ── 卡片2: 任务数 ── */
    int tasks = (int)uxTaskGetNumberOfTasks();
    snprintf(b, sizeof b, "%d", tasks);
    ui_set_label(l_task_big, b);

    /* ── 卡片3: CPU ── */
    int cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    snprintf(b, sizeof b, "%dMHz", cpu_mhz);
    ui_set_label(l_cpu_big, b);
    seg_set(cpu_seg, cpu_mhz * 100 / 240);   /* 相对 240MHz 满标 */
    static size_t s_psram_mb = 0;
    if (s_psram_mb == 0) s_psram_mb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024*1024);
    size_t psram_mb = s_psram_mb;
    snprintf(b, sizeof b, "Flash 16M  PSRAM %uM", (unsigned)psram_mb);
    ui_set_label(l_cpu_sub, b);

    /* ── 卡片4: WiFi ── */
    bool night = wifi_is_night_sleep();
    bool wok   = wifi_is_connected();
    int rssi = 0, ch = 0;
    wifi_get_ap_info(&rssi, &ch);   /* 一次查询取回 RSSI+信道 */
    const char *ssid = wifi_get_ssid();
    int lvl = rssi_level(rssi, wok, night);
    ui_sig_set(sig_bar, 4, lvl);
    if (night) {
        ui_set_label(l_wifi_ssid, "Zzz sleep");
        ui_set_label(l_wifi_sub, "night off");
    } else if (wok) {
        snprintf(b, sizeof b, "%.12s", (ssid && ssid[0]) ? ssid : "-");
        ui_set_label(l_wifi_ssid, b);
        snprintf(b, sizeof b, "%d dBm  ch%d", rssi, ch);
        ui_set_label(l_wifi_sub, b);
    } else {
        ui_set_label(l_wifi_ssid, "disconnected");
        ui_set_label(l_wifi_sub, "--- dBm");
    }
}

lv_obj_t *sysinfo_get_screen(void)
{
    return scr;
}

/**
 * @file ui_sysinfo.cpp — 第三屏: 系统/网络监控 (终端风 + 原生图形指示)
 *
 * 文本用 Montserrat (ASCII, 无字库风险).
 * CPU/RAM 用原生 LVGL 矩形画进度条, WiFi 信号用 4 根递增竖条 —— 不依赖任何
 * 特殊字符字形, 反射屏黑白矩形锐利清晰.
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
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "ui_sysinfo.h"
#include "wifi_app.h"
#include "api_clients.h"
#include "secrets.h"

static const char *TAG = "SYS";

#define C_BLACK lv_color_black()
#define C_WHITE lv_color_white()

#define FONT_S  (&lv_font_montserrat_12)
#define FONT_M  (&lv_font_montserrat_14)

/* 进度条几何: 起点 x / 宽 / 高 */
#define BAR_X     54
#define BAR_W     150
#define BAR_H     12
/* 信号竖条: 4 根, 每根宽 6, 间隔 3, 最高 16 */
#define SIG_BARS  4
#define SIG_W     6
#define SIG_GAP   3
#define SIG_HMAX  16

static lv_obj_t *scr;
static lv_obj_t *l_title;
static lv_obj_t *l_cpu, *l_ram, *l_heap, *l_chip;
static lv_obj_t *l_wifi, *l_ip, *l_ntp, *l_mode, *l_reset;

/* CPU/RAM 进度条: 外框 + 填充块 + 右侧数值标签 */
static lv_obj_t *cpu_fill, *ram_fill;
static lv_obj_t *cpu_val, *ram_val;
/* WiFi 行右侧文本 (SSID/dBm/ch) */
static lv_obj_t *wifi_txt;
/* WiFi 4 根信号竖条 */
static lv_obj_t *sig_bar[SIG_BARS];

/* 建标签 */
static lv_obj_t *mk(lv_obj_t *p, const char *t, lv_coord_t x, lv_coord_t y, const lv_font_t *f)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, C_BLACK, 0);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

/* 只在文本变化时刷新 (静止零重绘) */
static void set_label(lv_obj_t *l, const char *txt)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (cur && strcmp(cur, txt) == 0) return;
    lv_label_set_text(l, txt);
}

/* 画一个实心黑矩形 (无边框无圆角) */
static lv_obj_t *rect(lv_obj_t *p, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                      bool filled)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_set_size(r, w, h);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_scrollbar_mode(r, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    if (filled) {
        lv_obj_set_style_bg_color(r, C_BLACK, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(r, C_BLACK, 0);
        lv_obj_set_style_border_width(r, 1, 0);
    }
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, x, y);
    return r;
}

/* 进度条: 外空框 + 内填充块 (填充块宽度后续按百分比设) */
static lv_obj_t *progress(lv_obj_t *p, lv_coord_t x, lv_coord_t y)
{
    rect(p, x, y, BAR_W, BAR_H, false);                 /* 外框 */
    lv_obj_t *fill = rect(p, x + 1, y + 1, 1, BAR_H - 2, true); /* 填充块 */
    return fill;
}

/* 设进度条填充宽度 (0~100) */
static void progress_set(lv_obj_t *fill, int pct)
{
    if (!fill) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = (BAR_W - 2) * pct / 100;
    if (w < 1) w = 1;
    lv_obj_set_width(fill, w);
}

void sysinfo_create(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, C_WHITE, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    const int RH = 28;   /* 行距: 11 行均匀铺满整屏 (300px) */
    int y = 8;
    l_title = mk(scr, "rlcd@esp32-s3 ------- [ONLINE]", 6, y, FONT_M);
    y += 26;

    /* 顶部分隔线 */
    lv_obj_t *ln1 = lv_obj_create(scr);
    lv_obj_set_size(ln1, 388, 1);
    lv_obj_set_style_bg_color(ln1, C_BLACK, 0);
    lv_obj_set_style_border_width(ln1, 0, 0);
    lv_obj_align(ln1, LV_ALIGN_TOP_LEFT, 6, y);
    y += 10;

    /* 系统区: CPU/RAM 用图形进度条, 前缀标签 + 条 + 右侧数值 */
    l_cpu = mk(scr, "CPU", 6, y, FONT_M);
    cpu_fill = progress(scr, BAR_X, y + 1);
    cpu_val = mk(scr, "---MHz", BAR_X + BAR_W + 8, y, FONT_M);
    y += RH;
    l_ram = mk(scr, "RAM", 6, y, FONT_M);
    ram_fill = progress(scr, BAR_X, y + 1);
    ram_val = mk(scr, "--%", BAR_X + BAR_W + 8, y, FONT_M);
    y += RH;
    l_heap = mk(scr, "HEAP  min ---K  TASKS --",     6, y, FONT_M); y += RH;
    l_chip = mk(scr, "CHIP  --  FLASH --M PSRAM --M", 6, y, FONT_M); y += RH;

    /* 中部分隔线 */
    lv_obj_t *ln2 = lv_obj_create(scr);
    lv_obj_set_size(ln2, 388, 1);
    lv_obj_set_style_bg_color(ln2, C_BLACK, 0);
    lv_obj_set_style_border_width(ln2, 0, 0);
    lv_obj_align(ln2, LV_ALIGN_TOP_LEFT, 6, y);
    y += 10;

    /* 网络区: WIFI 行的信号用 4 根竖条 (放在 WIFI 文字右侧) */
    l_wifi = mk(scr, "WIFI", 6, y, FONT_M);
    /* 4 根竖条基线对齐到文字底部 */
    int sx = 54;
    /* 竖条底边固定基线 (与 WIFI 文字垂直居中): 4 根同底, 高度递增 = 标准信号格 */
    int base_y = y + 18;
    for (int i = 0; i < SIG_BARS; i++) {
        int h = 4 + i * 4;   /* 4/8/12/16, 底对齐, 阶梯上升 */
        sig_bar[i] = rect(scr, sx + i * (SIG_W + SIG_GAP), base_y - h, SIG_W, h, false);
        /* 底色设黑: update 里点亮(bg_opa=COVER)才显示为黑块, 否则用默认色=隐形 */
        lv_obj_set_style_bg_color(sig_bar[i], C_BLACK, 0);
    }
    /* 竖条右侧显示 SSID / dBm / 信道 */
    wifi_txt = mk(scr, "--------  ---dBm ch--", sx + SIG_BARS * (SIG_W + SIG_GAP) + 8, y, FONT_M);
    y += RH;
    l_ip    = mk(scr, "IP    ---.---.---.---",             6, y, FONT_M); y += RH;
    l_ntp   = mk(scr, "NTP   --   UP --h--m",              6, y, FONT_M); y += RH;
    l_mode  = mk(scr, "MODE  ---     RESET ---",           6, y, FONT_M); y += RH;
    l_reset = mk(scr, "IDF   v-.-.-",                      6, y, FONT_M);

    ESP_LOGI(TAG, "sysinfo screen created");
}

/* RSSI → 点亮几根竖条 (0~4) */
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

    /* ── 系统 ── */
    /* CPU: 固定频率, 进度条按 240MHz 满标, 数值显示在条右侧 */
    int cpu_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    progress_set(cpu_fill, cpu_mhz * 100 / 240);
    snprintf(b, sizeof b, "%dMHz", cpu_mhz);
    set_label(cpu_val, b);

    /* RAM: 已用百分比 */
    size_t r_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL)
                   + heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t r_free  = esp_get_free_heap_size();
    size_t r_used  = (r_total > r_free) ? r_total - r_free : 0;
    int used_pct   = (r_total > 0) ? (int)(r_used * 100 / r_total) : 0;
    progress_set(ram_fill, used_pct);
    snprintf(b, sizeof b, "%d%%", used_pct);
    set_label(ram_val, b);

    /* HEAP 历史最低 + 任务数 */
    size_t min_free = esp_get_minimum_free_heap_size();
    int tasks = (int)uxTaskGetNumberOfTasks();
    snprintf(b, sizeof b, "HEAP  min %uK  TASKS %d",
             (unsigned)(min_free / 1024), tasks);
    set_label(l_heap, b);

    /* 芯片型号/核数 + Flash/PSRAM */
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    size_t psram_mb = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024*1024);
    snprintf(b, sizeof b, "CHIP  S3 x%d  16M/%uM",
             ci.cores, (unsigned)psram_mb);
    set_label(l_chip, b);

    /* ── 网络 ── */
    bool night = wifi_is_night_sleep();
    bool wok   = wifi_is_connected();
    int rssi   = wifi_get_rssi();
    int ch     = wifi_get_channel();
    const char *ssid = wifi_get_ssid();

    /* 信号竖条: 点亮 lvl 根 (实心), 其余空心 */
    int lvl = rssi_level(rssi, wok, night);
    for (int i = 0; i < SIG_BARS; i++) {
        bool on = (i < lvl);
        lv_obj_set_style_bg_opa(sig_bar[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sig_bar[i], on ? 0 : 1, 0);
    }
    /* 竖条右侧文本: SSID / dBm / 信道 (夜间省电特别标注) */
    if (night)
        snprintf(b, sizeof b, "Zzz sleep");
    else if (wok)
        snprintf(b, sizeof b, "%.10s  %ddBm ch%d", (ssid && ssid[0]) ? ssid : "-", rssi, ch);
    else
        snprintf(b, sizeof b, "disconnected");
    set_label(wifi_txt, b);

    /* IP */
    snprintf(b, sizeof b, "IP    %s", wifi_get_ip());
    set_label(l_ip, b);

    /* NTP 时间 + uptime */
    time_t t; time(&t);
    struct tm ti; localtime_r(&t, &ti);
    uint64_t us = esp_timer_get_time();
    unsigned up_h = (unsigned)(us / 3600000000ULL);
    unsigned up_m = (unsigned)((us % 3600000000ULL) / 60000000ULL);
    if (ti.tm_year >= (2024 - 1900))
        snprintf(b, sizeof b, "NTP   %02d:%02d  UP %uh%02um", ti.tm_hour, ti.tm_min, up_h, up_m);
    else
        snprintf(b, sizeof b, "NTP   --     UP %uh%02um", up_h, up_m);
    set_label(l_ntp, b);

    /* 省电模式 (复位原因对日常无用, 已去掉) */
    snprintf(b, sizeof b, "MODE  %s", night ? "NIGHT (WiFi off)" : "DAY");
    set_label(l_mode, b);

    /* IDF 版本 */
    snprintf(b, sizeof b, "IDF   %s", esp_get_idf_version());
    set_label(l_reset, b);
}

lv_obj_t *sysinfo_get_screen(void)
{
    return scr;
}

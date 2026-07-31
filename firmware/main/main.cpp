/**
 * @file main.cpp
 * @brief RLCD Monitor 主入口
 *
 * 基金监控 · 天气 · 黄金 · DeepSeek Token 用量
 * Board: Waveshare ESP32-S3-RLCD-4.2
 *
 * 职责: 启动编排 + 三个业务任务 (sensor/api/ui) + Bridge 自动发现.
 * 电池采样/趋势 → battery.cpp; AP 配网 (DNS/HTTP 门户) → provisioning.cpp.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_pm.h>
#include <nvs_flash.h>
#include <nvs.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_config.h"
#include "secrets.h"

#include "wifi_app.h"
#include "shtc3.h"
#include "pcf85063.h"
#include "api_clients.h"
#include "battery.h"
#include "provisioning.h"
#include "ui_main.h"
#include "ui_dashboard.h"
#include "ui_sysinfo.h"

static const char *TAG = "MAIN";

/* 左键 GPIO18: 官方板载第二按键 (右键=BOOT/GPIO0). 低电平有效.
 * 右键: 循环切屏. 左键: 短按(<2s)重启网络, 长按(≥2s)重启设备. */
#define KEY2_GPIO           GPIO_NUM_18
#define KEY2_HOLD_TICKS     40   /* 长按重启阈值: 40 轮 × 50ms = 2 秒 */

/* 第一屏中文字库 (提示框用, 复用 ui_app 已链接的全量库) */
LV_FONT_DECLARE(custom_font_16_big);

#define PAGE_COUNT 3
static int g_page = 0;          /* 0=主界面 1=四宫格 2=系统监控 */
static bool g_key_last = true;  /* 右键 GPIO0 默认高电平 */
static bool g_key2_last = true; /* 左键 GPIO18 默认高电平 */
static lv_obj_t *main_screen = NULL;

/* 在当前活动屏中央弹出一个提示条 (黑底白字), 供重启/重连反馈.
 * 调用方须持有 LVGL 锁; 返回的对象由调用方决定是否删除 (重启场景无需删). */
static lv_obj_t *ui_toast(const char *txt)
{
    lv_obj_t *box = lv_obj_create(lv_scr_act());
    lv_obj_set_size(box, 220, 56);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_set_style_text_font(l, &custom_font_16_big, 0);
    lv_obj_center(l);
    return box;
}

/* 按页号取屏幕对象 */
static lv_obj_t *screen_for_page(int p)
{
    switch (p) {
        case 0:  return main_screen;
        case 1:  return dashboard_get_screen();
        default: return sysinfo_get_screen();
    }
}

DisplayPort RlcdPort(RLCD_MOSI_PIN, RLCD_SCK_PIN, RLCD_DC_PIN,
                     RLCD_CS_PIN, RLCD_RST_PIN, LCD_WIDTH, LCD_HEIGHT);

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
            /* 合理性检查：温度 -10~85°C（板子发热可能偏高），湿度 0~100%.
             * SHTC3 出错可能给 -40 之类离谱值, 超范围整帧丢弃, 保留上次好值 */
            if (hum >= 0 && hum <= 100 && temp >= -10 && temp <= 85) {
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

/* ===== 夜间省电测试开关 =====
 * 置 1: 用"分钟奇偶"造快速循环 (偶数分=白天, 奇数分=夜间), 每分钟切换一次,
 *       烧录后 1~2 分钟即可在串口看到进/出省电全过程, 无需等到 21 点.
 * 置 0: 正式的 21:00~07:00 窗口.
 * ⚠️ 测试完务必改回 0 再烧录. */
#define NIGHT_TEST_MODE 0

/* 夜间省电时段判断: 21:00~07:00 无业务数据需求, 关 WiFi 省电.
 * 用本地时间小时数判断; NTP 未同步(年份<2024)时视为白天, 避免误入省电. */
static bool is_night(void)
{
    time_t now = 0;
    time(&now);
    struct tm ti = {0};
    localtime_r(&now, &ti);
    if (ti.tm_year < (2024 - 1900)) return false;  /* 时间没同步, 不省电 */
#if NIGHT_TEST_MODE
    return (ti.tm_min % 2) != 0;   /* 测试: 奇数分钟=夜间, 每分钟切换 */
#else
    int h = ti.tm_hour;
    return (h >= 21 || h < 7);   /* 21:00~06:59 为夜间 */
#endif
}

/* NTP 对时成功后把系统时间回写 RTC, 供断网/夜间/掉电后恢复 */
static void rtc_save_from_system(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    if (ti.tm_year < (2024 - 1900)) return;   /* 系统时间还不可信, 不污染 RTC */
    if (pcf85063_write_time(&ti) == ESP_OK)
        ESP_LOGI(TAG, "RTC synced from NTP: %04d-%02d-%02d %02d:%02d",
                 ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
}

/* ===== 夜间省电: 电源管理档位 (档位一 — 只夜间开 light-sleep) =====
 * 白天 pm_set_night(false): 关 light-sleep + 定频 160MHz, 与不开 PM 时行为一致(白天零变化).
 * 夜间 pm_set_night(true):  开自动 light-sleep + 动态调频 40~160MHz. 此时 WiFi 已关,
 *   是最安全的 light-sleep 场景; CPU 空闲即由官方框架自动睡下, 省掉整夜 160MHz 空转.
 * 保守起见睡眠时 CPU 不断电(sdkconfig POWER_DOWN_CPU_IN_LIGHT_SLEEP=n), 避开八线 PSRAM
 *   缓存恢复的不稳点; 底噪略高但换稳定. */
static void pm_set_night(bool night)
{
    esp_pm_config_t cfg = {
        .max_freq_mhz = 160,
        .min_freq_mhz = night ? 40 : 160,
        .light_sleep_enable = night,
    };
    esp_err_t e = esp_pm_configure(&cfg);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "esp_pm_configure(%s)=%s", night ? "night" : "day", esp_err_to_name(e));
}

/* ===== API 轮询任务 ===== */
static void api_task(void *pv)
{
    /* 设置负偏移让首次请求立即触发 */
    TickType_t last_weather = -pdMS_TO_TICKS(3600000); /* 立即触发 */
    TickType_t last_fund = -pdMS_TO_TICKS(3600000); /* 首次立即触发 (负>30分) */
    TickType_t last_gold = -pdMS_TO_TICKS(3600000);
    TickType_t last_ds = -pdMS_TO_TICKS(3600000);
    /* NTP 每 24h 重新同步一次, 防止晶振长期漂移 (开机已同步过, 故初值=now).
     * 注意: 不能写 pdMS_TO_TICKS(24*3600*1000) — 其内部 ms*HZ 乘积(8.64e10)溢出
     * 32 位 TickType_t, 回绕后仅 ~500s. 直接算 tick 数(24*3600*HZ=86.4e6, 不溢出). */
    const TickType_t NTP_INTERVAL_TICKS = (TickType_t)24 * 3600 * configTICK_RATE_HZ;
    TickType_t last_ntp = xTaskGetTickCount();

    /* 夜间省电状态: 进入夜间关 WiFi 射频 + 开 light-sleep(CPU 空闲自动睡), 白天恢复.
     * 白天不睡(与原行为一致), 只在 WiFi 已关的夜间开睡眠 — 最安全的省电场景. */
    static bool s_in_night = false;

    while (1) {
        TickType_t now = xTaskGetTickCount();

        /* ===== 夜间省电时间门控 (21:00–07:00) =====
         * 仅在 NTP 对过时(时间可信)后才据此判断, 否则时间不准会误判. */
        bool night = is_night();
        if (night && !s_in_night) {
            /* 进入夜间: 先关射频, 再开 light-sleep (顺序保证不出现 WiFi+睡眠并存) */
            s_in_night = true;
            wifi_radio_off();
            pm_set_night(true);
            ESP_LOGI(TAG, "==> Night power-save (21:00-07:00): WiFi off + light-sleep");
        } else if (!night && s_in_night) {
            /* 出夜间: 先退 light-sleep 档, 再重开射频 (同样避免 WiFi+睡眠并存) */
            ESP_LOGI(TAG, "==> Day resume: light-sleep off, WiFi on, refetching...");
            pm_set_night(false);
            if (wifi_radio_on() == ESP_OK) {
                s_in_night = false;
                /* 负偏移让各项立即重拉; NTP 也立即重同步 */
                last_weather = last_fund = last_gold = last_ds = -pdMS_TO_TICKS(3600000);
                last_ntp = now - NTP_INTERVAL_TICKS;
            } else {
                /* 开 WiFi 失败: 回夜间省电档继续等, 下一轮(5s后)再试, 不卡死 */
                pm_set_night(true);
            }
        }

        /* 夜间: 射频已关, 跳过所有联网请求, 只维持时钟/屏幕显示 */
        if (s_in_night) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        bool wifi_ok = wifi_is_connected();
        wifi_check_reconnect();

        /* 每 24 小时重新对时 — 需 WiFi */
        if (wifi_ok && now - last_ntp >= NTP_INTERVAL_TICKS) {
            last_ntp = now;
            ESP_LOGI(TAG, "24h NTP re-sync...");
            if (ntp_sync_time() == ESP_OK) rtc_save_from_system();
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
    /* 左键状态: 按住轮数(每轮50ms)区分短按/长按; 短按重连的提示条到点自动删除 */
    int        key2_hold  = 0;
    lv_obj_t  *net_toast  = NULL;
    TickType_t toast_hide = 0;
    /* 短按重启网络: 锁内只建提示+置标志, 出锁后再做阻塞式射频操作 (避免持 LVGL 锁调 esp_wifi_stop/start) */
    bool       net_restart_pending = false;
    /* 右键切屏意图: 锁外置位, 锁内消费后清零. 若某轮 Lvgl_lock 超时(反射屏全屏刷新占锁),
     * 标志保留到下一轮再切, 不会随一次性边沿丢失 (与 net_restart_pending 同款做法). */
    bool       page_switch_pending = false;

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
            if (battery_update(&bat_tmp)) {
                DATA_LOCK();
                g_app_data.battery_pct    = bat_tmp.battery_pct;
                g_app_data.bat_drop_per_h = bat_tmp.bat_drop_per_h;
                g_app_data.bat_est_hours  = bat_tmp.bat_est_hours;
                g_app_data.bat_log_count  = bat_tmp.bat_log_count;
                DATA_UNLOCK();
            } else {
                ESP_LOGW(TAG, "ADC battery read failed, keep last value");
            }
        }

        bool key2 = (gpio_get_level(KEY2_GPIO) == 0);

        /* ── 按键边沿/计时全部在锁外算 (纯变量运算, 不受 LVGL 锁超时影响, 不丢键) ──
         * 锁内只消费这里算好的"意图"标志去操作 LVGL 对象. 原先整段包在锁内, 反射屏
         * 全屏刷新长时间持锁会导致 Lvgl_lock(100) 超时, 那一轮按键状态完全不更新→丢键,
         * 且 key2_hold 只在成功持锁时 ++, "2秒"漂移成"40次成功持锁". */
        bool page_edge  = (key && !g_key_last);      /* 右键下降沿: 切屏 */
        bool long_fire  = false;                     /* 左键长按到点: 重启设备 */
        bool short_fire = false;                     /* 左键短按松开: 重启网络 */
        if (page_edge) page_switch_pending = true;   /* 边沿转成待处理标志, 拿不到锁也不丢 */
        if (key2) {
            key2_hold++;
            /* >= 而非 == : key2_hold 在锁外自增, 若到点那轮恰好拿不到锁,
             * long_fire 未被消费, 下一轮 key2_hold 已超过阈值; 用 >= 保证仍会触发 */
            if (key2_hold >= KEY2_HOLD_TICKS) long_fire = true;
        } else {
            if (g_key2_last && key2_hold > 0 && key2_hold < KEY2_HOLD_TICKS)
                short_fire = true;
            key2_hold = 0;
        }
        g_key_last  = key;
        g_key2_last = key2;

        if (Lvgl_lock(100)) {
            bool page_switched = false;
            /* 右键: 循环切屏 0→1→2→0 (消费待处理标志, 消费成功才清零) */
            if (page_switch_pending) {
                page_switch_pending = false;
                g_page = (g_page + 1) % PAGE_COUNT;
                lv_scr_load_anim(screen_for_page(g_page),
                                  LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
                ESP_LOGI(TAG, "Next page %d", g_page);
                page_switched = true;
            }

            /* 左键长按: 重启设备 (弹提示→放锁→延时让提示刷出→重启) */
            if (long_fire) {
                ESP_LOGW(TAG, "Left key long-press: restarting device");
                ui_toast("正在重启...");
                Lvgl_unlock();
                vTaskDelay(pdMS_TO_TICKS(800));
                esp_restart();
            }

            /* 左键短按: 重启网络 (夜间只提示不动射频; 白天置标志出锁执行) */
            if (short_fire) {
                if (net_toast) lv_obj_del(net_toast);
                if (is_night()) {
                    ESP_LOGW(TAG, "Left key short-press ignored (night power-save)");
                    net_toast  = ui_toast("夜间省电中");
                    toast_hide = now + pdMS_TO_TICKS(2000);
                } else {
                    ESP_LOGW(TAG, "Left key short-press: restarting network");
                    /* 第一段: "重启网络..."; 射频重启完成后 (锁外) 会把文字换成结果并延长,
                     * 用"文字变化"给用户明确的前→后感知. 这里给个较长的兜底隐藏时间,
                     * 万一重启后重新加锁失败, toast 也不会永久残留. */
                    net_toast  = ui_toast("重启网络...");
                    toast_hide = now + pdMS_TO_TICKS(4000);
                    net_restart_pending = true;
                }
            }

            /* 网络提示条到点自动删除 */
            if (net_toast && now >= toast_hide) {
                lv_obj_del(net_toast);
                net_toast = NULL;
            }

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
                else if (g_page == 1)
                    dashboard_update(&local_data);
                else
                    sysinfo_update(&local_data);
            }

            Lvgl_unlock();
        }

        /* 短按重启网络: 在锁外执行阻塞式射频操作 (esp_wifi_stop/start 会阻塞几十~几百ms),
         * 避免持 LVGL 锁时卡住渲染任务; 提示条已在锁内先建好, 用户能立即看到. */
        if (net_restart_pending) {
            net_restart_pending = false;
            /* 原子重启射频: wifi_restart_radio 内部持射频锁完成 off→on, 并在锁内重判夜间.
             * 若执行到这里时已跨过 21:00 且 api_task 已切夜间省电, 则跳过, 不会把射频
             * 整夜打开 (修 H3+M1: 原来 ui_task 无锁 radio_off()+radio_on() 与 api_task 竞态). */
            wifi_restart_radio(is_night());
            /* 第二段反馈: 重启完成后把提示文字从"重启网络..."换成结果.
             * 反射屏无背光, 静止的框容易被忽略, 靠"文字变化"制造可感知的前→后差异,
             * 并延长停留到 2.5s 给用户明确的"完成"确认. 需重新加锁改 UI. */
            if (Lvgl_lock(200)) {
                if (net_toast) {
                    lv_obj_t *tl = lv_obj_get_child(net_toast, 0);
                    if (tl) lv_label_set_text(tl, wifi_is_connected() ? "网络已重启" : "重连中...");
                    toast_hide = xTaskGetTickCount() + pdMS_TO_TICKS(2500);
                }
                Lvgl_unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

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
            /* 校验: 只接受 http(s):// 开头且长度合理的 URL. UDP:7777 无认证,
             * 同网段任何设备都能发包, 加前缀/长度校验挡掉畸形或恶意内容 (SSRF/劫持数据源). */
            size_t ulen = strlen(url);
            if ((strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
                || ulen < 11 || ulen >= 128) {
                ESP_LOGW(TAG, "Discovery: reject invalid URL");
                continue;
            }
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

/* ===== 主入口 ===== */
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

    /* 时区固定东八区 (与网络无关, 提前设好, 让 RTC/NTP/localtime 全程一致) */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* RTC: 板载 PCF85063 有电池保时. 开机先用它兜底系统时钟 (断网/夜间/掉电重启也有正确
     * 时间), NTP 成功后再回写校准. */
    pcf85063_init();
    {
        struct tm rtm;
        if (pcf85063_read_time(&rtm) == ESP_OK) {
            time_t rt = mktime(&rtm);
            struct timeval tv = { .tv_sec = rt, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "RTC time restored: %04d-%02d-%02d %02d:%02d",
                     rtm.tm_year + 1900, rtm.tm_mon + 1, rtm.tm_mday, rtm.tm_hour, rtm.tm_min);
        } else {
            ESP_LOGW(TAG, "RTC invalid/absent, will rely on NTP");
        }
    }

    /* 4. 初始化 UI */
    if (Lvgl_lock(-1)) {
        ui_init();
        main_screen = lv_scr_act();
        Lvgl_unlock();
    }

    /* 初始化按键: 右键 GPIO0 (BOOT) 循环切屏 + 左键 GPIO18 短按重启网络/长按重启设备. 均上拉输入, 低电平有效 */
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << KEY_GPIO) | (1ULL << KEY2_GPIO);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_config(&io_conf);
    g_key_last  = gpio_get_level(KEY_GPIO);
    g_key2_last = gpio_get_level(KEY2_GPIO);

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
        if (ntp_sync_time() == ESP_OK) rtc_save_from_system();   /* 对时成功回写 RTC */
    } else {
        ESP_LOGW(TAG, "WiFi failed, AP mode");
        start_ap_provision();
    }

    /* 6. 初始化仪表盘 + 系统监控页 (持 LVGL 锁: 此时 LVGL 任务已在运行, 避免创建对象与渲染竞争) */
    if (Lvgl_lock(-1)) {
        dashboard_create();
        sysinfo_create();
        Lvgl_unlock();
    }

    /* 7. 创建数据锁 (必须在任务启动前) */
    s_data_mutex = xSemaphoreCreateMutex();
    if (!s_data_mutex) ESP_LOGE(TAG, "data mutex create fail");

    /* 电源管理基线: 白天档 (关 light-sleep, 定频 160MHz — 行为与开机前一致).
     * 夜间由 api_task 切到 light-sleep 档. */
    pm_set_night(false);

    /* 8. 启动任务 */
    xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(api_task,    "api",    8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(ui_task,     "ui",     5120, NULL, 6, NULL, 0);
    xTaskCreatePinnedToCore(bridge_discovery_task, "discovery", 4096, NULL, 1, NULL, 1);

    ESP_LOGI(TAG, "=== All systems running ===");
    ESP_LOGI(TAG, "Type 'bridge://IP:PORT' to change Bridge URL");
}

/**
 * @file battery.cpp
 * @brief 电池电量 (ADC 采样 + Li-ion 查表) 与续航趋势估算 — 从 main.cpp 拆出
 *
 * 电量% 由 ADC1_CH3(GPIO4) 读取; 趋势(每小时掉电/预估续航)纯 RAM 维护, 不落 NVS.
 * 旧版把 72 条记录反复写 16KB 的 NVS, 碎片化后 nvs_set 返回 NOT_ENOUGH_SPACE(0x1105),
 * 导致配网 WiFi 凭证存不进去. 趋势属非关键数据, 改纯 RAM (重启重置), 彻底不占 NVS.
 */

#include <cstring>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <nvs.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "battery.h"

static const char *TAG = "BAT";

/* ===== 电池电量读取 (ADC1_CH3 = GPIO4) =====
 * 换算链: 原始码值 -> adc_cali 曲线拟合校准电压(逐芯片准, 消除 ADC 偏移/增益误差)
 *         -> ×分压比 -> 电池 mV -> Li-ion 电压曲线插值 -> 电量%.
 * 旧版直接用原始码值套魔法表, 不同芯片/批次会系统性偏; 改为校准电压后逐芯片准.
 *
 * BAT_DIVIDER: 本板 (Waveshare ESP32-S3-RLCD-4.2) 分压系数, 已单点校准.
 * 设计值 3 (4.2V 电池经 /3 得 ~1.4V, 落在 12dB 衰减线性量程内), 但实测偏低:
 * 充满(CHG 绿灯熄灭)时 ADC 只读 4018mV, 真实应 ~4200mV, 故 ×(4200/4018)=1.0453,
 * 修正为 3×1.0453≈3.136. 若换电池/换板不准, 可再单点校正: 充满时看串口 batt mV,
 * 系数 = 4200 / 打印值, 乘到此处. (STAT 未引到 GPIO, 满电只能靠板载绿灯判断) */
#define BAT_DIVIDER  3.136f

static adc_oneshot_unit_handle_t s_adc_handle  = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;

/* Li-ion 电压(mV)->电量(%) 曲线: 两头陡、中段平, 贴合实际放电特性(比线性映射准得多) */
static const struct { int mv; int pct; } k_bat_curve[] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3850, 60},
    {3790,  50}, {3750, 40}, {3710, 30}, {3670, 20}, {3610, 10},
    {3500,   5}, {3400,  2}, {3300,  1}, {3200,  0}, {3000,  0},
};

/* 读一次电池电压(mV): 采 11 次校准值中值滤波; 全部失败返回 0 */
static int battery_read_mv(void)
{
    if (!s_adc_handle) {
        adc_oneshot_unit_init_cfg_t init_cfg = {};
        init_cfg.unit_id = ADC_UNIT_1;
        if (adc_oneshot_new_unit(&init_cfg, &s_adc_handle) != ESP_OK) {
            ESP_LOGE(TAG, "ADC unit init fail");
            return 0;
        }
        adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_3, &chan_cfg);
    }
    if (!s_cali_handle) {
        /* 曲线拟合校准: 用芯片 eFuse 出厂校准数据, 逐芯片消除 ADC 偏移/增益误差 */
        adc_cali_curve_fitting_config_t cali_cfg = {};
        cali_cfg.unit_id  = ADC_UNIT_1;
        cali_cfg.atten    = ADC_ATTEN_DB_12;
        cali_cfg.bitwidth = ADC_BITWIDTH_12;
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) != ESP_OK) {
            ESP_LOGE(TAG, "ADC cali init fail");
            return 0;
        }
    }

    /* 采 11 次: 每次原始码值 -> 校准 mV -> ×分压. 排序去掉最高/最低取中间均值,
     * 滤除 WiFi 发射瞬间(拉 100~300mA)造成的电压瞬跌污染. */
    int mv[11], n = 0;
    for (int i = 0; i < 11; i++) {
        int raw = 0, pin_mv = 0;
        if (adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali_handle, raw, &pin_mv) == ESP_OK) {
            mv[n++] = (int)(pin_mv * BAT_DIVIDER + 0.5f);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (n == 0) return 0;
    /* 插入排序 (n<=11) */
    for (int i = 1; i < n; i++) {
        int v = mv[i], j = i - 1;
        while (j >= 0 && mv[j] > v) { mv[j+1] = mv[j]; j--; }
        mv[j+1] = v;
    }
    int lo = (n > 4) ? 1 : 0, hi = (n > 4) ? n - 1 : n, sum = 0;
    for (int i = lo; i < hi; i++) sum += mv[i];
    return sum / (hi - lo);
}

int battery_read_pct(void)
{
    int mv = battery_read_mv();
    if (mv <= 0) return -1;   /* 采样/校准全失败: 调用方保留上次好值 */
    ESP_LOGI(TAG, "batt: %d mV", mv);   /* 供两点校准对照(见文件头 BAT_DIVIDER 说明) */

    /* 低电迟滞: <3200mV 锁 0%, 需回升到 >3450mV 才解锁, 避免末端在 0 附近来回跳 */
    static bool s_low_latched = false;
    if (s_low_latched) {
        if (mv < 3450) return 0;
        s_low_latched = false;
    } else if (mv < 3200) {
        s_low_latched = true;
        return 0;
    }

    /* Li-ion 电压曲线插值 */
    const int N = (int)(sizeof(k_bat_curve) / sizeof(k_bat_curve[0]));
    if (mv >= k_bat_curve[0].mv) return 100;
    for (int i = 1; i < N; i++) {
        if (mv >= k_bat_curve[i].mv) {
            int pct = k_bat_curve[i].pct +
                      (k_bat_curve[i-1].pct - k_bat_curve[i].pct) * (mv - k_bat_curve[i].mv) /
                      (k_bat_curve[i-1].mv - k_bat_curve[i].mv);
            if (pct < 0)   pct = 0;
            if (pct > 100) pct = 100;
            return pct;
        }
    }
    return 0;
}

/* ===== 电池电量趋势 (纯 RAM, 不落 NVS) ===== */
#define BAT_LOG_INTERVAL_MS  600000   /* 每10分钟采一次趋势 (>=2 次才出趋势, 即约 20 分钟后) */
#define BAT_LOG_MAX          72

static int32_t s_bat_log_count = 0;  /* 已采样次数 (>=2 才出趋势) */
static int s_bat_peak_pct = -1;      /* 最高电量 (充满基准) */
static uint32_t s_bat_peak_ts = 0;
static int s_prev_pct = -1;          /* 上一轮电量 (判电量是否回升=充电) */

void battery_nvs_cleanup(void)
{
    nvs_handle_t h;
    if (nvs_open("battery", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "old battery NVS cleared (reclaim space)");
    }
}

static void battery_log_save(int pct)
{
    time_t now;
    time(&now);
    if (now < 100000) return; /* NTP未同步, 时间不可靠 */

    /* 距上次采样够间隔才更新 (RAM 计时, 不落 NVS) */
    static uint32_t s_last_ts = 0;
    if (s_last_ts && (uint32_t)now - s_last_ts < BAT_LOG_INTERVAL_MS / 1000) return;
    s_last_ts = (uint32_t)now;

    if (s_bat_log_count < BAT_LOG_MAX) s_bat_log_count++;

    /* 更新峰值: 仅当明显上升(>=3%)才视为充电, 忽略ADC波动 */
    if (s_bat_peak_pct < 0 || pct > s_bat_peak_pct + 3) {
        s_bat_peak_pct = pct;
        s_bat_peak_ts = (uint32_t)now;
    }
}

static void battery_calc_trend(AppData_t *app)
{
    app->bat_log_count = (int)s_bat_log_count;

    if (s_bat_log_count < 2 || s_bat_peak_ts == 0) {
        app->bat_drop_per_h = 0;
        app->bat_est_hours = 999;
        return;
    }

    /* 用峰值对比当前 */
    uint32_t now = (uint32_t)time(NULL);
    int dropped = s_bat_peak_pct - app->battery_pct;

    /* 检测充电: 唯一可靠信号是"电量真的回升"(充电会抬高电压 -> pct 上升).
     * 硬件 STAT 未引到 GPIO, 无法直读充电状态; 原先的 dropped<=0(在峰值即判充电)会在
     * 满电/刚拔线时误报, 已去掉, 只保留回升判据. 用插电前电量(prev)作起始, 充满才跳100. */
    int prev_pct = s_prev_pct;   /* 先存旧值: 下面要用"插电前"电量, 不能被当前值覆盖 */
    s_prev_pct = app->battery_pct;
    bool is_charging = (prev_pct >= 0 && app->battery_pct > prev_pct + 2);

    if (is_charging) {
        app->bat_drop_per_h = -1;   /* -1 = 充电中 (UI 据此显示"充电") */
        app->bat_est_hours = 999;
        return;
    }

    /* 时间回退保护: now/peak_ts 均 uint32, 若 NTP 校时往回拨导致 now < peak_ts,
     * 相减会下溢成巨大值 -> elapsed_h 巨大 -> 趋势失真. 此时以当前为新峰值基准重来. */
    if (now < s_bat_peak_ts) {
        s_bat_peak_ts = now;
        app->bat_drop_per_h = 0;
        return;
    }

    float elapsed_h = (float)(now - s_bat_peak_ts) / 3600.0f;
    if (elapsed_h < 0.5f) { /* 刚拔USB不到30分钟, 数据太少 (Li-ion电压还稳定) */
        app->bat_drop_per_h = 0;   /* 0 = 无趋势(非充电); 不再用 -1, 避免误显示"充电中" */
        return;
    }

    int per_h = (int)((float)dropped / elapsed_h + 0.5f);
    if (per_h < 1) per_h = 1;

    app->bat_drop_per_h = per_h;
    app->bat_est_hours = app->battery_pct / per_h;

    ESP_LOGI(TAG, "Trend: peak=%d%%, now=%d%%, drop=%d%% in %.1fh = %d%%/h",
             s_bat_peak_pct, app->battery_pct, dropped, elapsed_h, per_h);
}

bool battery_update(AppData_t *app)
{
    int pct = battery_read_pct();
    /* ADC 全采样失败返回 -1: 跳过本轮 log/trend, 保留上次好值.
     * 否则 -1 会污染峰值基准, 且 trend 里 dropped=(-1)-(-1)=0 误判为"充电" */
    if (pct < 0) return false;
    app->battery_pct = pct;
    battery_log_save(pct);
    battery_calc_trend(app);
    return true;
}

/**
 * @file pcf85063.cpp
 * @brief PCF85063 硬件 RTC 驱动 (I2C) — 断电由纽扣电池保时
 *
 * ESP32 内部 RTC 掉电即丢, 且夜间关 WiFi / 断网时无法 NTP 对时. 板载 PCF85063
 * 有独立电池, 开机可据此恢复系统时钟; NTP 成功后再回写校准. 与 SHTC3 共用 I2C_NUM_0.
 *
 * 寄存器: 0x04 秒(bit7=VL 振荡器停止标志) / 0x05 分 / 0x06 时(24h) / 0x07 日 /
 *         0x08 星期 / 0x09 月 / 0x0A 年. 数据为 BCD.
 */

#include <cstring>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include "driver/i2c.h"
#include "pcf85063.h"
#include "shtc3.h"   /* I2C_MASTER_NUM */

static const char *TAG = "RTC";

#define REG_CTRL1    0x00
#define REG_SECONDS  0x04   /* bit7 = VL: 置位表示振荡器曾停止, 时间不可信 */

static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_write_read_device(I2C_MASTER_NUM, PCF85063_ADDR,
                                        &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

static esp_err_t reg_write(uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[8];
    if (len > sizeof(tmp) - 1) return ESP_ERR_INVALID_ARG;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    return i2c_master_write_to_device(I2C_MASTER_NUM, PCF85063_ADDR,
                                      tmp, len + 1, pdMS_TO_TICKS(100));
}

esp_err_t pcf85063_init(void)
{
    /* Control_1 = 0x00: 正常模式, RTC 运行, 24 小时制, 无软复位 */
    uint8_t c1 = 0x00;
    esp_err_t e = reg_write(REG_CTRL1, &c1, 1);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "init fail (PCF85063 absent? %s)", esp_err_to_name(e));
    else
        ESP_LOGI(TAG, "PCF85063 init OK");
    return e;
}

esp_err_t pcf85063_read_time(struct tm *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t b[7] = {0};
    esp_err_t e = reg_read(REG_SECONDS, b, sizeof(b));
    if (e != ESP_OK) return e;
    if (b[0] & 0x80) return ESP_FAIL;   /* VL=1: 掉电过, 时间不可信 */

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(b[0] & 0x7F);
    out->tm_min  = bcd2dec(b[1] & 0x7F);
    out->tm_hour = bcd2dec(b[2] & 0x3F);
    out->tm_mday = bcd2dec(b[3] & 0x3F);
    out->tm_wday = b[4] & 0x07;
    out->tm_mon  = bcd2dec(b[5] & 0x1F) - 1;   /* 1-12 → 0-11 */
    out->tm_year = bcd2dec(b[6]) + 100;        /* 20xx → 距 1900 的年数 */
    out->tm_isdst = 0;

    /* 合理性: 2024~2099 才接受 */
    if (out->tm_year < (2024 - 1900) || out->tm_year > (2099 - 1900))
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t pcf85063_write_time(const struct tm *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    uint8_t b[7];
    b[0] = dec2bcd((uint8_t)t->tm_sec) & 0x7F;   /* 同时把 VL 写 0, 标记时间可信 */
    b[1] = dec2bcd((uint8_t)t->tm_min);
    b[2] = dec2bcd((uint8_t)t->tm_hour);
    b[3] = dec2bcd((uint8_t)t->tm_mday);
    b[4] = (uint8_t)(t->tm_wday & 0x07);
    b[5] = dec2bcd((uint8_t)(t->tm_mon + 1));
    b[6] = dec2bcd((uint8_t)(t->tm_year % 100));
    return reg_write(REG_SECONDS, b, sizeof(b));
}

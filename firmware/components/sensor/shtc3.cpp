/**
 * @file shtc3.cpp
 * @brief SHTC3 温湿度传感器驱动 (I2C)
 */

#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>
#include "shtc3.h"

static const char *TAG = "SHTC3";

/* 芯片自热补偿: 传感器与主板同 PCB, 热平衡后读数稳定偏高.
 * 真实室温 = 传感器读数 + TEMP_OFFSET (标定值, 需实测微调).
 * 标定方法: 设备运行满 20 分钟热平衡后, 用准确温度计对比,
 *           TEMP_OFFSET = 真实室温 - 设备显示. */
static float s_temp_offset = -23.0f;

/* 热平衡时间(秒): 冷启动补偿从 0 渐进到满偏移的时长 (与上面 20 分钟标定条件一致) */
#define SHTC3_WARMUP_SEC (20 * 60)

/* SHTC3 命令 */
#define SHTC3_CMD_WAKEUP      0x3517
#define SHTC3_CMD_SLEEP       0xB098
#define SHTC3_CMD_MEASURE     0x7CA2  /* 时钟拉伸使能, 低功耗 */

void i2c_master_init(void)
{
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = I2C_MASTER_SDA_PIN;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = I2C_MASTER_SCL_PIN;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER,
                                       0, 0, 0));
    ESP_LOGI(TAG, "I2C initialized");
}

static esp_err_t shtc3_write_cmd(uint16_t cmd)
{
    uint8_t buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF)};
    return i2c_master_write_to_device(I2C_MASTER_NUM, SHTC3_ADDR,
                                       buf, 2, pdMS_TO_TICKS(100));
}

static uint8_t shtc3_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

esp_err_t shtc3_read(float *temperature, float *humidity)
{
    uint8_t buf[6] = {0};

    /* 唤醒 */
    shtc3_write_cmd(SHTC3_CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(1));

    /* 触发测量 */
    shtc3_write_cmd(SHTC3_CMD_MEASURE);
    vTaskDelay(pdMS_TO_TICKS(15));

    /* 读结果: 湿度2字节 + CRC + 温度2字节 + CRC */
    esp_err_t ret = i2c_master_read_from_device(I2C_MASTER_NUM, SHTC3_ADDR,
                                                 buf, 6, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read failed: %d", ret);
        return ret;
    }

    /* CRC 校验 */
    if (shtc3_crc8(buf, 2) != buf[2] || shtc3_crc8(buf + 3, 2) != buf[5]) {
        ESP_LOGW(TAG, "CRC error");
        return ESP_FAIL;
    }

    /* 转换 */
    uint16_t raw_hum = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_temp = ((uint16_t)buf[3] << 8) | buf[4];

    *humidity    = 100.0f * raw_hum / 65536.0f;
    *temperature = -45.0f + 175.0f * raw_temp / 65536.0f;

    /* 合理性检测 (用原始读数判断, 补偿前) */
    if (*temperature < 0 || *temperature > 70 || *humidity < 0 || *humidity > 100) {
        ESP_LOGW(TAG, "Bad reading: %.1fC %.1f%%RH", *temperature, *humidity);
        return ESP_FAIL;
    }

    /* 芯片自热补偿:
     * 平衡态自热恒为 |s_temp_offset| (由硬件功耗决定, 标定得到).
     * 只有"冷上电"后约 SHTC3_WARMUP_SEC 内芯片仍在升温, 此时按开机时间线性渐进,
     * 避免刚上电就减满偏移得到荒谬低温/负温.
     * 软重启(如配网后 esp_restart)/panic/看门狗复位时板子已热平衡, 立即用满偏移不再渐进
     * —— 用复位原因区分冷/热启动, 避免热重启后 20 分钟内温度显示偏高. */
    float eff_offset = s_temp_offset;
    if (esp_reset_reason() == ESP_RST_POWERON) {
        int64_t up_s = esp_timer_get_time() / 1000000;
        if (up_s < SHTC3_WARMUP_SEC) {
            eff_offset = s_temp_offset * (float)up_s / (float)SHTC3_WARMUP_SEC;
        }
    }

    *temperature += eff_offset;

    /* 休眠 */
    shtc3_write_cmd(SHTC3_CMD_SLEEP);

    return ESP_OK;
}

/* 运行时设置自热偏移 (标定用): 真实室温 = 显示 + offset */
void shtc3_set_temp_offset(float offset_c)
{
    s_temp_offset = offset_c;
    ESP_LOGI(TAG, "Temp offset set: %.1fC", offset_c);
}
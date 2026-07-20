/**
 * @file shtc3.cpp
 * @brief SHTC3 温湿度传感器驱动 (I2C)
 */

#include <cstring>
#include <esp_log.h>
#include "shtc3.h"

static const char *TAG = "SHTC3";
static float s_ambient = 0;  /* 室温基准 (开机早期设定) */

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

    /* 芯片发热补偿: 高于基准的降回基准 (由 shtc3_set_baseline 设定) */
    if (s_ambient > 0.5f && *temperature > s_ambient) {
        *temperature = s_ambient;
    }

    /* 合理性检测 */
    if (*temperature < 0 || *temperature > 70 || *humidity < 0 || *humidity > 100) {
        ESP_LOGW(TAG, "Bad reading: %.1fC %.1f%%RH", *temperature, *humidity);
        return ESP_FAIL;
    }



    /* 休眠 */
    shtc3_write_cmd(SHTC3_CMD_SLEEP);

    return ESP_OK;
}

/* 设置室温基准 (app_main 在 WiFi 启动前调用) */
void shtc3_set_baseline(float temp_c)
{
    s_ambient = temp_c;
    ESP_LOGI(TAG, "Baseline set: %.1fC", temp_c);
}
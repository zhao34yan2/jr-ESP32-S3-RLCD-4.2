#ifndef SHTC3_H
#define SHTC3_H

#include "esp_err.h"
#include "driver/i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C 硬件配置 — Waveshare ESP32-S3-RLCD-4.2 */
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_SDA_PIN  GPIO_NUM_13
#define I2C_MASTER_SCL_PIN  GPIO_NUM_14
#define I2C_MASTER_FREQ_HZ  400000
#define SHTC3_ADDR          0x70

/**
 * @brief 初始化 I2C 主机 (用于 SHTC3 + PCF85063)
 */
void i2c_master_init(void);

/**
 * @brief 读取 SHTC3 温湿度
 * @param temperature 输出温度 (°C)
 * @param humidity    输出湿度 (%RH)
 * @return ESP_OK 成功
 */
esp_err_t shtc3_read(float *temperature, float *humidity);

/**
 * @brief 设置芯片自热补偿偏移 (真实室温 = 传感器读数 + offset)
 * @param offset_c 偏移量 (°C), 通常为负值; 热平衡后实测标定
 */
void shtc3_set_temp_offset(float offset_c);

#ifdef __cplusplus
}
#endif

#endif /* SHTC3_H */
#ifndef PCF85063_H
#define PCF85063_H

#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PCF85063 RTC I2C 地址 (与 user_config.h 一致, 用守卫避免重复定义冲突) */
#ifndef PCF85063_ADDR
#define PCF85063_ADDR   0x51
#endif

/**
 * @brief 初始化 PCF85063 (正常运行模式, 24 小时制). I2C 须已 i2c_master_init().
 * @return ESP_OK 成功; 失败多为芯片不在总线上
 */
esp_err_t pcf85063_init(void);

/**
 * @brief 读 RTC 时间 → struct tm (本地时间字段).
 * @return ESP_OK; 振荡器曾停止(VL=1)或年份不合理时返回 ESP_FAIL (时间不可信)
 */
esp_err_t pcf85063_read_time(struct tm *out);

/**
 * @brief 把 struct tm 写入 RTC (并清 VL 位, 标记时间可信).
 */
esp_err_t pcf85063_write_time(const struct tm *t);

#ifdef __cplusplus
}
#endif

#endif /* PCF85063_H */

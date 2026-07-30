#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/* ============================================================
 * 硬件引脚配置 — Waveshare ESP32-S3-RLCD-4.2
 * ============================================================ */

/* ST7305 RLCD 显示 (4.2", 400x300 反射式) */
#define LCD_WIDTH       400
#define LCD_HEIGHT      300

#define RLCD_DC_PIN     GPIO_NUM_5
#define RLCD_CS_PIN     GPIO_NUM_40
#define RLCD_SCK_PIN    GPIO_NUM_11
#define RLCD_MOSI_PIN   GPIO_NUM_12
#define RLCD_RST_PIN    GPIO_NUM_41
#define RLCD_TE_PIN     GPIO_NUM_6

/* I2C 总线 — 板载 SHTC3 + PCF85063 */
#define I2C_MASTER_NUM      I2C_NUM_0
#define I2C_MASTER_SDA_PIN  GPIO_NUM_13
#define I2C_MASTER_SCL_PIN  GPIO_NUM_14
#define I2C_MASTER_FREQ_HZ  400000

/* SHTC3 温湿度传感器 */
#define SHTC3_ADDR          0x70

/* PCF85063 RTC */
#define PCF85063_ADDR       0x51

/* 按键 */
#define KEY_GPIO            GPIO_NUM_0   /* BOOT 按钮 */

/* ============================================================
 * 软件配置
 * ============================================================ */

/* 数据刷新间隔 (秒) */
#define WEATHER_REFRESH_SEC     1800    /* 30 分钟 */
#define GOLD_REFRESH_SEC        1800    /* 30 分钟 */
#define FUND_REFRESH_SEC        1800    /* 30 分钟 */
#define DEEPSEEK_REFRESH_SEC    300     /* 5 分钟 */
#define SENSOR_REFRESH_SEC      60      /* 1 分钟 */

#endif /* USER_CONFIG_H */

#ifndef WIFI_APP_H
#define WIFI_APP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi STA 模式并连接
 * @param ssid WiFi SSID
 * @param password WiFi 密码
 */
void wifi_init_sta(const char *ssid, const char *password);

/**
 * @brief 等待 WiFi 连接完成
 * @param timeout_ms 超时毫秒
 * @return ESP_OK 连接成功，ESP_FAIL 超时
 */
esp_err_t wifi_wait_connected(int timeout_ms);

/**
 * @brief 获取 WiFi 连接状态
 * @return true 已连接
 */
bool wifi_is_connected(void);

/**
 * @brief WiFi 断线重连检查 (每2分钟自动尝试)
 */
void wifi_check_reconnect(void);

/**
 * @brief 停止 STA 自动重连 (进 AP 配网前调用, 避免重连与扫描抢射频)
 */
void wifi_stop_sta_reconnect(void);

/**
 * @brief 切换到另一个网络 (WiFi 已初始化后调用, 不重建驱动)
 * @param ssid     新 SSID
 * @param password 新密码 (空串=开放网络)
 */
void wifi_switch_network(const char *ssid, const char *password);

/**
 * @brief 同步 NTP 时间
 * @return ESP_OK 成功
 */
esp_err_t ntp_sync_time(void);

/**
 * @brief 获取当前时间字符串
 * @param buf 输出缓冲区 (至少 24 字节)
 */
void get_time_str(char *buf);

/**
 * @brief 获取当前日期字符串
 * @param buf 输出缓冲区 (至少 16 字节)
 */
void get_date_str(char *buf);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_APP_H */
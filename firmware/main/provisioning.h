#ifndef PROVISIONING_H
#define PROVISIONING_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 AP 配网模式 (阻塞)
 *
 * 开启 RLCD-AP 开放热点 + DNS 劫持 (Captive Portal) + HTTP 配置门户,
 * 用户在网页填 WiFi 凭证存入 NVS 后自动重启. 5 分钟无操作超时也重启.
 * WiFi 连不上时由 app_main 调用.
 */
void start_ap_provision(void);

#ifdef __cplusplus
}
#endif

#endif /* PROVISIONING_H */

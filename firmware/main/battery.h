#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>
#include "api_clients.h"   /* AppData_t */

#ifdef __cplusplus
extern "C" {
#endif

/* 开机一次性清理旧版 battery NVS 命名空间 (回收被撑碎的空间, 让配网凭证能写入) */
void battery_nvs_cleanup(void);

/* 读电量% (ADC1_CH3=GPIO4, 校准电压×分压 + Li-ion 电压曲线 + 11 次中值滤波); 全采样失败返回 -1 */
int battery_read_pct(void);

/* 读电量 + 算续航趋势, 写入 app 的电池字段 (battery_pct / bat_drop_per_h / bat_est_hours /
 * bat_log_count). 内部按 10 分钟节流采样趋势. ADC 失败返回 false (调用方应保留上次好值). */
bool battery_update(AppData_t *app);

#ifdef __cplusplus
}
#endif

#endif /* BATTERY_H */

#ifndef UI_SYSINFO_H
#define UI_SYSINFO_H

#include "api_clients.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 第三屏: 系统/网络监控 (终端 + 进度条风格, 全 ASCII 避开精简字库) */
void sysinfo_create(void);
void sysinfo_update(const AppData_t *app);
lv_obj_t *sysinfo_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif

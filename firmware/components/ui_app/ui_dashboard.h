#ifndef UI_DASHBOARD_H
#define UI_DASHBOARD_H

#include "api_clients.h"

#ifdef __cplusplus
extern "C" {
#endif

void dashboard_create(void);
void dashboard_update(const AppData_t *app);
lv_obj_t *dashboard_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif
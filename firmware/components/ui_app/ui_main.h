#ifndef UI_MAIN_H
#define UI_MAIN_H

#include "api_clients.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 LVGL UI
 */
void ui_init(void);

/**
 * @brief 更新所有 UI 控件
 * @param app 当前应用数据
 */
void ui_update_all(const AppData_t *app);

#ifdef __cplusplus
}
#endif

#endif /* UI_MAIN_H */
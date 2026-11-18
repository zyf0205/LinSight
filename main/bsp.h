#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_LCD_H_RES 320
#define BSP_LCD_V_RES 240

/* I2C + PCA9557 + LCD + touch + LVGL. Backlight stays off. */
esp_err_t bsp_init(void);

/* 0..100, active-low backlight on GPIO42 */
esp_err_t bsp_backlight_set(int percent);

typedef enum {
    BSP_BUTTON_SHORT = 0,
    BSP_BUTTON_LONG,
} bsp_button_event_t;

typedef void (*bsp_button_cb_t)(bsp_button_event_t event);

/* GPIO0 user key, active-low. Long press is >= 800 ms; callback runs in a task. */
esp_err_t bsp_user_button_init(bsp_button_cb_t cb);

#ifdef __cplusplus
}
#endif

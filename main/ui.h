#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_CONN_WIFI = 0, /* searching for Wi-Fi */
    UI_CONN_HOST,     /* Wi-Fi up, connecting to the host */
    UI_CONN_ONLINE,   /* TCP session established */
    UI_CONN_RECONNECT,
} linsight_conn_t;

/* Call with the LVGL port locked. UI data (trace, stats) is pulled from the
 * capture core by a 20 Hz LVGL timer; the setters below only push events. */
void linsight_ui_init(void);

/* All setters lock internally; safe from any task. */
void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms);
void linsight_ui_toggle_settings(void); /* user key long press */

#ifdef __cplusplus
}
#endif

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

/* Call with the LVGL port locked. Stats and SD status are pulled by a 20 Hz
 * LVGL timer; the setters below only push external events. */
void linsight_ui_init(void);

/* All setters only publish into a lock-free mailbox that the 20 Hz LVGL timer
 * drains; they are non-blocking and safe from any task (net_task, GPIO). */
void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms);

/* Publish the active host endpoint (network byte order). Safe from net_task;
 * the 20 Hz timer updates the SETUP label. */
void linsight_ui_set_host_endpoint(uint32_t ip_be, uint16_t port);

/* GPIO0 user key: short press cycles DASH -> DIAG -> SETUP; long press runs
 * the current view action (DASH pause, DIAG clear stats, SETUP back to DASH).
 * When the backlight is off, the first press only wakes the display. */
void linsight_ui_handle_key(bool long_press);

#ifdef __cplusplus
}
#endif

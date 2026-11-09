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

/* Call with the LVGL port locked. Trace and stats are pulled by a 20 Hz LVGL
 * timer; the setters below only push external events. */
void linsight_ui_init(void);

/* All setters only publish into a lock-free mailbox that the 20 Hz LVGL timer
 * drains; they are non-blocking and safe from any task (net_task, GPIO). */
void linsight_ui_set_conn(linsight_conn_t state, uint32_t rtt_ms);

/* GPIO0 user key: short press toggles display pause, long press cycles the
 * views Overview -> Raw -> Settings -> Overview. */
void linsight_ui_handle_key(bool long_press);

/* Host CONTROL (0x07) was applied: show REMOTE SYNC. The refresh timer reads
 * authoritative capture state directly from twai. */
void linsight_ui_sync_control(void);

#ifdef __cplusplus
}
#endif

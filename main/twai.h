#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CAN bitrates supported by the Settings page and CONTROL.set_config. */
typedef enum {
    CAN_BITRATE_125K = 0,
    CAN_BITRATE_250K = 1,
    CAN_BITRATE_500K = 2,
    CAN_BITRATE_1M = 3,
} can_bitrate_t;

typedef enum {
    CAN_MODE_LISTEN_ONLY = 0, /* never ACKs / transmits; safe on unknown buses */
    CAN_MODE_NORMAL = 1,      /* ACKs the bus; bench use only */
    CAN_MODE_SELFTEST = 2,    /* ACKs its own frames; single-node load testing */
} can_mode_t;

typedef struct {
    uint32_t rx_total;      /* frames stored since boot (or since clear) */
    uint32_t dropped;       /* ring overwrites (can_ring) */
    uint32_t rx_missed;     /* lost in the TWAI RX FIFO */
    uint32_t rx_queue_full; /* software RX queue full alerts */
    uint32_t bus_errors;    /* bus error alerts */
    uint32_t bus_off;       /* bus-off events */
    uint32_t err_passive;   /* error-passive entries */
    uint32_t tec;           /* transmit error counter */
    uint32_t rec;           /* receive error counter */
    uint32_t fps;           /* frames received in the last full second */
    uint32_t load_permille; /* estimated bus load, 0..1000 */
    const char *state;      /* "RUNNING" / "BUS_OFF" / "RECOVERING" / "STOPPED" */
    bool paused;
} can_stats_t;

typedef struct {
    uint32_t ts_ms;
    const char *type; /* "bus_off" / "err_passive" / "rx_overflow" / "bus_error" */
} can_event_t;

/* Create the stats lock and event queue. Call once from app_main before the
 * UI starts: the 20 Hz refresh timer reads stats as soon as it exists. */
esp_err_t twai_capture_init(void);

/* Start the TWAI task on core 0. One instance only. */
esp_err_t twai_capture_start(can_bitrate_t bitrate, can_mode_t mode);

/* Stop and re-configure (Settings page / CONTROL.set_config). */
esp_err_t twai_capture_restart(can_bitrate_t bitrate, can_mode_t mode);

void twai_capture_set_paused(bool paused);

/* Copy the counters and drain at most one pending event. Thread-safe. */
void twai_stats_snapshot(can_stats_t *out);
bool twai_event_pop(can_event_t *out);

/* Reset the software counters (CONTROL.clear_stats); TWAI TEC/REC untouched. */
void twai_stats_clear(void);

const char *can_bitrate_name(can_bitrate_t br);   /* "500k" */
const char *can_mode_name(can_mode_t mode);       /* "LISTEN" / "NORMAL" */
can_bitrate_t twai_current_bitrate(void);
can_mode_t twai_current_mode(void);

#ifdef __cplusplus
}
#endif

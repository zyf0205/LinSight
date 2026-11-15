#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CAN bitrates supported by the Settings page. */
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
} can_stats_t;

typedef struct {
    uint32_t ts_ms;
    const char *type; /* "bus_off" / "err_passive" / "rx_overflow" / "bus_error" */
} can_event_t;

/* Create the stats lock and event queue. Call once from app_main before the
 * UI starts: the 20 Hz refresh timer reads stats as soon as it exists. */
esp_err_t twai_capture_init(void);

/* Start the TWAI task on core 1. One instance only. Called by app_main and by
 * twai_capture_restart() while it holds the restart lock, so this function
 * must never take that lock itself. */
esp_err_t twai_capture_start(can_bitrate_t bitrate, can_mode_t mode);

/* Stop and re-configure (Settings page). Serialized against other restarts
 * and manual recovery. */
esp_err_t twai_capture_restart(can_bitrate_t bitrate, can_mode_t mode);

/* Push an event into the host/UI event queue (SD mount/full/io, bus errors). */
void twai_event_push(const char *type);

/* Manual bus-off recovery (Settings "Bus Recover"). No-op while running. */
esp_err_t twai_capture_recover(void);

/* Copy the counters. Thread-safe. */
void twai_stats_snapshot(can_stats_t *out);

/* Drain at most one pending event. Thread-safe. */
bool twai_event_pop(can_event_t *out);

/* Copy the newest n captured records for the local UI (oldest first).
 * This is capture-side history, independent of the network ring: net_task
 * popping records for streaming must not blank or bias the trace. */
uint32_t twai_ui_tail(linsight_can_record_t *out, uint32_t n);

/* Reset the software counters (Settings Clear button); TWAI TEC/REC untouched. */
void twai_stats_clear(void);

const char *can_bitrate_name(can_bitrate_t br);   /* "500k" */
const char *can_mode_name(can_mode_t mode);       /* "LISTEN" / "NORMAL" */
can_bitrate_t twai_current_bitrate(void);
can_mode_t twai_current_mode(void);

#ifdef __cplusplus
}
#endif

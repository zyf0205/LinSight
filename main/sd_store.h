#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool mounted;   /* filesystem currently usable */
    bool recording; /* writer currently appending */
    uint32_t files; /* .log recordings retained on the card */
    uint32_t dropped;
    uint64_t free_kb;
} sd_status_t;

void sd_store_init(void);

/* Called from twai_task after can_ring_push(); never blocks. */
void sd_store_push(const linsight_can_record_t *rec);

/* Append a trigger/mark sidecar entry; safe from any task. Returns false when
 * no recording is open or the mark queue is full. */
bool sd_store_mark(const char *reason, uint32_t id, uint16_t seq);

/* Copy a cached snapshot; no SD I/O. */
void sd_store_get_status(sd_status_t *out);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>
#include <stdio.h>

#include "frame.h"

/*
 * .lsc binary log: 32-byte header + raw 20-byte records.
 * Header: magic "LSC1" | u16 version | u32 bitrate | u32 flags |
 *         u64 unix-seconds at session start | reserved to 32 bytes.
 * A truncated tail record is tolerated when reading (crash recovery).
 */

#define LSC_HEADER_SIZE 32u
#define LSC_VERSION 1u

typedef struct {
    FILE *f;
    uint64_t written;
    uint32_t bitrate;
    uint64_t start_wall; /* unix seconds at session start */
} lsc_writer_t;

int lsc_writer_open(lsc_writer_t *w, const char *path, uint32_t bitrate, uint64_t start_wall);
/* Returns 0 on success, -1 on write error. */
int lsc_writer_write(lsc_writer_t *w, const linsight_can_record_t *r);
int lsc_writer_flush(lsc_writer_t *w);
void lsc_writer_close(lsc_writer_t *w);

/* Returns the number of records passed to cb, or -1 on open error. */
typedef void (*lsc_record_cb)(const linsight_can_record_t *r, void *ctx);
long lsc_read(const char *path, lsc_record_cb cb, void *ctx);

/* Exports. Both return the number of frames written, or -1. */
long canlog_export_asc(const char *lsc_path, const char *out_path);
long canlog_export_candump(const char *lsc_path, const char *out_path);

/* Format one record as a candump line using the given session start time.
 * Returns the string length, or -1 when buf is too small. */
int canlog_format_candump(const linsight_can_record_t *r, uint64_t start_wall, char *buf,
                          size_t cap);

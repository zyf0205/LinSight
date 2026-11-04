#include "canlog.h"

#include <string.h>

static const char LSC_MAGIC[4] = {'L', 'S', 'C', '1'};

int lsc_writer_open(lsc_writer_t *w, const char *path, uint32_t bitrate, uint64_t start_wall)
{
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "wb");
    if (!w->f) {
        return -1;
    }

    uint8_t hdr[LSC_HEADER_SIZE] = {0};
    memcpy(hdr, LSC_MAGIC, 4);
    hdr[4] = LSC_VERSION & 0xFFu;
    hdr[5] = (LSC_VERSION >> 8) & 0xFFu;
    linsight_put_u32(hdr + 6, bitrate);
    linsight_put_u32(hdr + 10, 0); /* flags */
    for (int i = 0; i < 8; i++) {
        hdr[16 + i] = (uint8_t)(start_wall >> (8 * i));
    }

    w->bitrate = bitrate;
    w->start_wall = start_wall;
    if (fwrite(hdr, 1, LSC_HEADER_SIZE, w->f) != LSC_HEADER_SIZE) {
        fclose(w->f);
        w->f = NULL;
        return -1;
    }
    return 0;
}

int lsc_writer_write(lsc_writer_t *w, const linsight_can_record_t *r)
{
    uint8_t buf[LINSIGHT_REC_SIZE];
    linsight_record_pack(r, buf);
    if (fwrite(buf, 1, LINSIGHT_REC_SIZE, w->f) != LINSIGHT_REC_SIZE) {
        return -1;
    }
    w->written++;
    return 0;
}

int lsc_writer_flush(lsc_writer_t *w)
{
    return fflush(w->f) == 0 ? 0 : -1;
}

void lsc_writer_close(lsc_writer_t *w)
{
    if (w->f) {
        fflush(w->f);
        fclose(w->f);
        w->f = NULL;
    }
}

long lsc_read(const char *path, lsc_record_cb cb, void *ctx)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }

    uint8_t hdr[LSC_HEADER_SIZE];
    if (fread(hdr, 1, LSC_HEADER_SIZE, f) != LSC_HEADER_SIZE ||
        memcmp(hdr, LSC_MAGIC, 4) != 0) {
        fclose(f);
        return -1;
    }

    long count = 0;
    uint8_t buf[LINSIGHT_REC_SIZE];
    linsight_can_record_t rec;
    while (fread(buf, 1, LINSIGHT_REC_SIZE, f) == LINSIGHT_REC_SIZE) {
        linsight_record_unpack(buf, &rec);
        cb(&rec, ctx);
        count++;
    }
    /* A short read means a truncated tail record: tolerated by design. */
    fclose(f);
    return count;
}

/* ------------------------------------------------------------ exports */

typedef struct {
    FILE *out;
    uint64_t start_wall;
    uint32_t bitrate;
    long count;
} export_ctx_t;

/* Vector-ASC-ish: "<wallclock> <bitrate-k>  <ID>[x] Rx   d|r <dlc> <data...>" */
static void export_asc_cb(const linsight_can_record_t *r, void *ctx)
{
    export_ctx_t *e = ctx;
    const double ts = (double)e->start_wall + (double)r->ts_ms / 1000.0;
    const char ext = (r->flags & LINSIGHT_REC_FLAG_EXT) ? 'x' : ' ';
    const char type = (r->flags & LINSIGHT_REC_FLAG_RTR) ? 'r' : 'd';

    if (r->flags & LINSIGHT_REC_FLAG_EXT) {
        fprintf(e->out, "%.6f %u  %08X%c Rx   %c %u", ts, (unsigned)e->bitrate / 1000u,
                (unsigned)r->id, ext, type, (unsigned)r->dlc);
    } else {
        fprintf(e->out, "%.6f %u    %03X%c Rx   %c %u", ts, (unsigned)e->bitrate / 1000u,
                (unsigned)r->id, ext, type, (unsigned)r->dlc);
    }
    for (int i = 0; i < r->dlc; i++) {
        fprintf(e->out, " %02X", r->data[i]);
    }
    fputc('\n', e->out);
    e->count++;
}

static void export_candump_cb(const linsight_can_record_t *r, void *ctx)
{
    export_ctx_t *e = ctx;
    const double ts = (double)e->start_wall + (double)r->ts_ms / 1000.0;

    fprintf(e->out, "(%.6f) can0 %s%08X", ts, (r->flags & LINSIGHT_REC_FLAG_EXT) ? "" : "   ",
            (unsigned)r->id);
    if (r->flags & LINSIGHT_REC_FLAG_RTR) {
        fprintf(e->out, " [R] ");
        for (int i = 0; i < r->dlc; i++) {
            fprintf(e->out, " %02X", r->data[i]);
        }
    } else {
        fprintf(e->out, " [%u]", (unsigned)r->dlc);
        for (int i = 0; i < r->dlc; i++) {
            fprintf(e->out, " %02X", r->data[i]);
        }
    }
    fputc('\n', e->out);
    e->count++;
}

static long export_common(const char *lsc_path, const char *out_path, lsc_record_cb cb)
{
    export_ctx_t e = {0};
    e.out = (strcmp(out_path, "-") == 0) ? stdout : fopen(out_path, "w");
    if (!e.out) {
        return -1;
    }

    FILE *hdr_probe = fopen(lsc_path, "rb");
    if (!hdr_probe) {
        if (e.out != stdout) {
            fclose(e.out);
        }
        return -1;
    }
    uint8_t hdr[LSC_HEADER_SIZE];
    if (fread(hdr, 1, LSC_HEADER_SIZE, hdr_probe) != LSC_HEADER_SIZE) {
        fclose(hdr_probe);
        if (e.out != stdout) {
            fclose(e.out);
        }
        return -1;
    }
    fclose(hdr_probe);
    e.bitrate = linsight_get_u32(hdr + 6);
    for (int i = 0; i < 8; i++) {
        e.start_wall |= (uint64_t)hdr[16 + i] << (8 * i);
    }

    const long n = lsc_read(lsc_path, cb, &e);
    if (e.out != stdout) {
        fclose(e.out);
    }
    return n >= 0 ? e.count : -1;
}

long canlog_export_asc(const char *lsc_path, const char *out_path)
{
    return export_common(lsc_path, out_path, export_asc_cb);
}

long canlog_export_candump(const char *lsc_path, const char *out_path)
{
    return export_common(lsc_path, out_path, export_candump_cb);
}

int canlog_format_candump(const linsight_can_record_t *r, uint64_t start_wall, char *buf,
                          size_t cap)
{
    const double ts = (double)start_wall + (double)r->ts_ms / 1000.0;
    size_t used = (size_t)snprintf(buf, cap, "(%.6f) can0 %s%08X [%u]", ts,
                                   (r->flags & LINSIGHT_REC_FLAG_EXT) ? "" : "   ",
                                   (unsigned)r->id, (unsigned)r->dlc);
    if (used >= cap) {
        return -1;
    }
    for (int i = 0; i < r->dlc && used + 3 < cap; i++) {
        used += (size_t)snprintf(buf + used, cap - used, " %02X", r->data[i]);
    }
    return (int)used;
}

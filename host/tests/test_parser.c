/* Incremental parser tests: fragmentation, resync, limits, skip. */

#include <stdio.h>
#include <string.h>

#include "frame.h"

static int g_fail;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

typedef struct {
    uint8_t type[64];
    uint16_t len[64];
    uint8_t first[64];
    int n;
} seen_t;

static void cb(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    seen_t *s = ctx;
    if (s->n < 64) {
        s->type[s->n] = type;
        s->len[s->n] = len;
        s->first[s->n] = len ? payload[0] : 0;
        s->n++;
    }
}

static size_t make_frame(uint8_t *dst, uint8_t type, const void *payload, uint16_t len)
{
    const size_t n = linsight_frame_build(dst, 20000, type, payload, len);
    CHECK(n > 0);
    return n;
}

static void test_single_frame(void)
{
    const char payload[] = "{\"proto\":32}";
    uint8_t frame[64];
    make_frame(frame, LINSIGHT_FRAME_HELLO, payload, (uint16_t)strlen(payload));

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[256];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, frame, sizeof(frame), cb, &s) == LINSIGHT_PARSE_OK);
    CHECK(s.n == 1 && s.type[0] == LINSIGHT_FRAME_HELLO);
    CHECK(s.len[0] == strlen(payload) && s.first[0] == '{');
    CHECK(p.frames == 1);
}

static void test_byte_by_byte(void)
{
    const uint8_t payload[] = {0x42};
    uint8_t frame[16];
    const size_t flen = make_frame(frame, LINSIGHT_FRAME_PING, payload, 1);

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[64];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    for (size_t i = 0; i < flen; i++) {
        CHECK(linsight_parser_feed(&p, frame + i, 1, cb, &s) == LINSIGHT_PARSE_OK);
    }
    CHECK(s.n == 1 && s.type[0] == LINSIGHT_FRAME_PING && s.len[0] == 1);
    CHECK(s.first[0] == 0x42);
}

static void test_multiple_frames_one_feed(void)
{
    uint8_t buf[512];
    size_t off = 0;
    const char stats[] = "{\"fps\":100}";
    const uint8_t ping[] = {1};
    const uint8_t ack[] = {0x7B};

    linsight_frame_build(buf + off, sizeof(buf) - off, LINSIGHT_FRAME_STATS, stats,
                         (uint16_t)strlen(stats));
    off += LINSIGHT_FRAME_HEADER_SIZE + strlen(stats);
    linsight_frame_build(buf + off, sizeof(buf) - off, LINSIGHT_FRAME_PING, ping, 1);
    off += LINSIGHT_FRAME_HEADER_SIZE + 1;
    linsight_frame_build(buf + off, sizeof(buf) - off, LINSIGHT_FRAME_ACK, ack, 1);
    off += LINSIGHT_FRAME_HEADER_SIZE + 1;

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[256];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, buf, off, cb, &s) == LINSIGHT_PARSE_OK);
    CHECK(s.n == 3);
    CHECK(s.type[0] == LINSIGHT_FRAME_STATS && s.len[0] == strlen(stats));
    CHECK(s.type[1] == LINSIGHT_FRAME_PING && s.len[1] == 1);
    CHECK(s.type[2] == LINSIGHT_FRAME_ACK && s.len[2] == 1);
    CHECK(p.frames == 3);
}

static void test_resync_on_garbage(void)
{
    const char payload[] = "hi";
    uint8_t frame[32];
    const size_t flen = make_frame(frame, LINSIGHT_FRAME_HELLO, payload,
                                   (uint16_t)strlen(payload));

    /* Garbage precedes the frame; the parser should find the magic. */
    uint8_t buf[64] = {0x01, 0x02, 0xAA, 0x00, 0x55, 0x00};
    memcpy(buf + 6, frame, flen);

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[256];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, buf, sizeof(buf), cb, &s) == LINSIGHT_PARSE_OK);
    CHECK(s.n == 1 && s.type[0] == LINSIGHT_FRAME_HELLO);
    CHECK(p.resyncs >= 1);
}

static void test_limit_exceeded_is_fatal(void)
{
    /* A known type over its protocol limit is fatal at header time. */
    uint8_t frame[] = {0xAA, 0x55, LINSIGHT_FRAME_CONTROL, 200, 0x00};

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[256];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, frame, sizeof(frame), cb, &s) ==
          LINSIGHT_PARSE_ERROR);
    CHECK(s.n == 0);
    /* Stays fatal until reset. */
    CHECK(linsight_parser_feed(&p, frame, sizeof(frame), cb, &s) ==
          LINSIGHT_PARSE_ERROR);
    linsight_parser_reset(&p);
    CHECK(!p.fatal);
}

static void test_oversized_batch_skipped(void)
{
    /* A known type over its limit is fatal, but a CAN_BATCH larger than the
     * scratch buffer is skipped, not fatal (payload stays under its own cap). */
    static uint8_t frame[LINSIGHT_FRAME_HEADER_SIZE + 4096];
    linsight_frame_write_header(frame, LINSIGHT_FRAME_CAN_BATCH, 4096);
    memset(frame + LINSIGHT_FRAME_HEADER_SIZE, 0xEE, 4096);

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[256];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, frame, sizeof(frame), cb, &s) == LINSIGHT_PARSE_OK);
    CHECK(s.n == 0 && p.skipped == 1);

    /* A following valid frame must still arrive. */
    const uint8_t pong[] = {0};
    uint8_t next[16];
    const size_t nlen = make_frame(next, LINSIGHT_FRAME_PONG, pong, 1);
    CHECK(linsight_parser_feed(&p, next, nlen, cb, &s) == LINSIGHT_PARSE_OK);
    CHECK(s.n == 1 && s.type[0] == LINSIGHT_FRAME_PONG);
}

static void test_unknown_type_skipped(void)
{
    /* Unknown types are skipped silently (forward compatibility). */
    const uint8_t payload[] = {0xEE};
    uint8_t frame[16];
    const size_t flen = make_frame(frame, 0x7F, payload, 1);

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[256];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, frame, flen, cb, &s) == LINSIGHT_PARSE_OK);
    CHECK(s.n == 0 && p.skipped == 1);
}

static void test_can_batch_end_to_end(void)
{
    linsight_can_record_t recs[2] = {0};
    recs[0] = (linsight_can_record_t){.seq = 7, .ts_ms = 1234, .id = 0x456, .dlc = 2,
                                      .data = {0xAA, 0xBB}};
    recs[1] = (linsight_can_record_t){.seq = 8, .ts_ms = 1240,
                                      .id = 0x1ABCDEF,
                                      .flags = LINSIGHT_REC_FLAG_EXT, .dlc = 0};

    uint8_t batch[2 + 2 * LINSIGHT_REC_SIZE];
    const size_t blen = linsight_batch_pack(batch, sizeof(batch), recs, 2);

    uint8_t frame[128];
    make_frame(frame, LINSIGHT_FRAME_CAN_BATCH, batch, (uint16_t)blen);

    seen_t s = {0};
    linsight_parser_t p;
    uint8_t scratch[LINSIGHT_MAX_CAN_BATCH];
    linsight_parser_init(&p, scratch, sizeof(scratch));
    CHECK(linsight_parser_feed(&p, frame, LINSIGHT_FRAME_HEADER_SIZE + blen, cb, &s) ==
          LINSIGHT_PARSE_OK);
    CHECK(s.n == 1 && s.type[0] == LINSIGHT_FRAME_CAN_BATCH && s.len[0] == blen);
    CHECK(linsight_get_u16(scratch) == 2);

    /* Recover the original records from the delivered payload. */
    for (int i = 0; i < 2; i++) {
        linsight_can_record_t r;
        CHECK(linsight_record_unpack(scratch + 2 + (size_t)i * LINSIGHT_REC_SIZE, &r));
        CHECK(r.seq == recs[i].seq && r.id == recs[i].id && r.dlc == recs[i].dlc);
    }
}

int main(void)
{
    test_single_frame();
    test_byte_by_byte();
    test_multiple_frames_one_feed();
    test_resync_on_garbage();
    test_limit_exceeded_is_fatal();
    test_oversized_batch_skipped();
    test_unknown_type_skipped();
    test_can_batch_end_to_end();

    if (g_fail) {
        fprintf(stderr, "test_parser: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_parser: all passed\n");
    return 0;
}

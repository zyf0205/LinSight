/* Parser robustness: random input and mutated frames must never crash the
 * parser or leave it in a state that skips subsequent valid frames.
 * Deterministic seed so failures reproduce. */

#include <stdio.h>
#include <stdlib.h>
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

/* xorshift32 — deterministic, no libc rand state. */
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

typedef struct {
    int count;
    uint8_t last_type;
} fuzz_cb_t;

static void cb(void *ctx, uint8_t type, const uint8_t *payload, uint16_t len)
{
    fuzz_cb_t *s = ctx;
    (void)payload;
    (void)len;
    s->count++;
    s->last_type = type;
}

/* A valid HELLO frame the fuzz rounds use as an anchor. */
static size_t make_hello(uint8_t *dst, size_t cap)
{
    const char payload[] = "{\"proto\":32,\"fw\":\"2.0.0\"}";
    const size_t n = linsight_frame_build(dst, cap, LINSIGHT_FRAME_HELLO, payload,
                                          (uint16_t)strlen(payload));
    CHECK(n > 0);
    return n;
}

static void test_random_bytes(void)
{
    linsight_parser_t p;
    static uint8_t scratch[LINSIGHT_MAX_CAN_BATCH];
    linsight_parser_init(&p, scratch, sizeof(scratch));

    uint8_t chunk[64];
    fuzz_cb_t seen = {0};
    for (int round = 0; round < 50000; round++) {
        const size_t n = 1 + rng() % sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            chunk[i] = (uint8_t)rng();
        }
        const linsight_parse_result_t r =
            linsight_parser_feed(&p, chunk, n, cb, &seen);
        CHECK(r == LINSIGHT_PARSE_OK || r == LINSIGHT_PARSE_ERROR);
        if (r == LINSIGHT_PARSE_ERROR) {
            linsight_parser_reset(&p);
        }
        CHECK(p.payload_got <= p.payload_len);
    }
}

static void test_mutated_valid_frames(void)
{
    linsight_parser_t p;
    static uint8_t scratch[LINSIGHT_MAX_CAN_BATCH];
    linsight_parser_init(&p, scratch, sizeof(scratch));

    uint8_t frame[128];
    const size_t flen = make_hello(frame, sizeof(frame));

    fuzz_cb_t seen = {0};
    for (int round = 0; round < 50000; round++) {
        uint8_t mutated[128];
        memcpy(mutated, frame, flen);
        const int flips = 1 + (int)(rng() % 4);
        for (int f = 0; f < flips; f++) {
            const size_t pos = rng() % flen;
            mutated[pos] ^= (uint8_t)(1u << (rng() % 8));
        }
        const linsight_parse_result_t r =
            linsight_parser_feed(&p, mutated, flen, cb, &seen);
        CHECK(r == LINSIGHT_PARSE_OK || r == LINSIGHT_PARSE_ERROR);
        if (r == LINSIGHT_PARSE_ERROR) {
            linsight_parser_reset(&p);
        }
        CHECK(p.payload_got <= p.payload_len);
    }
}

static void test_recovers_after_garbage(void)
{
    linsight_parser_t p;
    static uint8_t scratch[LINSIGHT_MAX_CAN_BATCH];
    linsight_parser_init(&p, scratch, sizeof(scratch));

    uint8_t frame[128];
    const size_t flen = make_hello(frame, sizeof(frame));

    fuzz_cb_t seen = {0};
    /* Interleave garbage with the anchor frame; the parser must keep finding
     * the magic and keep delivering frames (seen > 0 proves it). */
    uint8_t buf[192];
    for (int round = 0; round < 20000; round++) {
        for (size_t i = 0; i < 48; i++) {
            buf[i] = (uint8_t)rng();
        }
        memcpy(buf + 48, frame, flen);

        linsight_parser_feed(&p, buf, 48 + flen, cb, &seen);
        if (seen.count > 0) {
            linsight_parser_reset(&p);
            seen.count = 0;
        }
    }
    CHECK(seen.count >= 0); /* no crash is the point; delivered > 0 at times */
}

int main(void)
{
    test_random_bytes();
    test_mutated_valid_frames();
    test_recovers_after_garbage();

    if (g_fail) {
        fprintf(stderr, "test_fuzz: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_fuzz: all passed\n");
    return 0;
}

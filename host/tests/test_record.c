/* Record pack/unpack + batch pack tests. */

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

static void test_pack_unpack_roundtrip(void)
{
    static const uint8_t data[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    const linsight_can_record_t in = {
        .seq = 0xABCD,
        .ts_ms = 0x12345678,
        .id = 0x1ABCDEFL,
        .flags = LINSIGHT_REC_FLAG_EXT | LINSIGHT_REC_FLAG_RTR,
        .dlc = 8,
        .data = {data[0], data[1], data[2], data[3], data[4], data[5], data[6],
                 data[7]},
    };

    uint8_t buf[LINSIGHT_REC_SIZE];
    linsight_record_pack(&in, buf);

    /* Field offsets per the wire contract. */
    CHECK(linsight_get_u16(buf + 0) == 0xABCD);
    CHECK(linsight_get_u32(buf + 2) == 0x12345678);
    CHECK(linsight_get_u32(buf + 6) == 0x1ABCDEFL);
    CHECK(buf[10] == (LINSIGHT_REC_FLAG_EXT | LINSIGHT_REC_FLAG_RTR));
    CHECK(buf[11] == 8);
    CHECK(buf[12] == 0xDE && buf[19] == 0x04);

    linsight_can_record_t out;
    CHECK(linsight_record_unpack(buf, &out));
    CHECK(out.seq == in.seq && out.ts_ms == in.ts_ms && out.id == in.id);
    CHECK(out.flags == in.flags && out.dlc == in.dlc);
    CHECK(memcmp(out.data, in.data, 8) == 0);
}

static void test_unpack_rejects_bad_dlc(void)
{
    uint8_t buf[LINSIGHT_REC_SIZE] = {0};
    buf[11] = 9; /* dlc > 8 */
    linsight_can_record_t r;
    CHECK(!linsight_record_unpack(buf, &r));
    /* Struct still filled in for logging. */
    CHECK(r.dlc == 9);
}

static void test_batch_pack(void)
{
    linsight_can_record_t recs[3];
    memset(recs, 0, sizeof(recs));
    for (int i = 0; i < 3; i++) {
        recs[i].seq = (uint16_t)i;
        recs[i].ts_ms = 100u + (uint32_t)i;
        recs[i].id = 0x100 + (uint32_t)i;
        recs[i].dlc = (uint8_t)i;
    }

    uint8_t buf[LINSIGHT_MAX_CAN_BATCH];
    const size_t n = linsight_batch_pack(buf, sizeof(buf), recs, 3);
    CHECK(n == 2 + 3 * LINSIGHT_REC_SIZE);
    CHECK(linsight_get_u16(buf) == 3);

    for (int i = 0; i < 3; i++) {
        linsight_can_record_t r;
        CHECK(linsight_record_unpack(buf + 2 + (size_t)i * LINSIGHT_REC_SIZE, &r));
        CHECK(r.seq == (uint16_t)i && r.ts_ms == 100u + (uint32_t)i);
        CHECK(r.id == 0x100 + (uint32_t)i && r.dlc == (uint8_t)i);
    }
}

static void test_batch_pack_rejects(void)
{
    linsight_can_record_t r = {0};
    uint8_t small[10];
    CHECK(linsight_batch_pack(small, sizeof(small), &r, 1) == 0); /* cap too small */
    CHECK(linsight_batch_pack(small, sizeof(small), NULL, 0) == 0); /* count 0 */
    /* Max batch: 819 records must fit, 820 must not. */
    static linsight_can_record_t many[820];
    static uint8_t big[LINSIGHT_MAX_CAN_BATCH];
    CHECK(linsight_batch_pack(big, sizeof(big), many, 819) ==
          2 + 819 * LINSIGHT_REC_SIZE);
    CHECK(linsight_batch_pack(big, sizeof(big), many, 820) == 0);
}

int main(void)
{
    test_pack_unpack_roundtrip();
    test_unpack_rejects_bad_dlc();
    test_batch_pack();
    test_batch_pack_rejects();

    if (g_fail) {
        fprintf(stderr, "test_record: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_record: all passed\n");
    return 0;
}

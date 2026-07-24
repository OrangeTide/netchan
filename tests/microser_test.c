/* microser_test.c : round-trip and edge tests for the microser codecs */
/* PUBLIC DOMAIN (CC0-1.0) */

/*
 * microser_proto.idl is compiled to microser_proto.c/.h by the build, and
 * this drives the result: every scalar wire type, zero-copy bytes and
 * string, the discriminated union in each of its variants, a message at the
 * maximum field number, a buffer too small to write into, a truncated buffer
 * to read from, and a forward-compatible reader stepping over a field a newer
 * writer added.
 */

#include "microser.h"
#include "microser_proto.h"

#include <stdio.h>
#include <string.h>

static int tests_run;
static int tests_passed;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  %-50s ", name); \
        fflush(stdout); \
    } while (0)

#define PASS() \
    do { \
        tests_passed++; \
        printf("OK\n"); \
    } while (0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
        return; \
    } while (0)

#define CHECK(cond, msg) \
    do { if (!(cond)) FAIL(msg); } while (0)

static void
test_every_scalar(void)
{
    TEST("every scalar type round-trips");

    uint8_t buf[256];
    struct everything a = {
        .u8v = 0xff, .i8v = -128,
        .u16v = 0xbeef, .i16v = -32768,
        .u32v = 0xdeadbeef, .i32v = -2147483647 - 1,
        .u64v = 0xfeedfacecafebeefULL, .i64v = -9223372036854775807LL - 1,
        .blob = (const uint8_t *)"\x01\x02\x03\x04", .blob_len = 4,
        .name = "microser", .name_len = 8,
    };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    CHECK(n > 0, "encode failed");
    CHECK(everything_decode(&b, buf, n) == n, "decode did not consume all");
    CHECK(b.u8v == a.u8v && b.i8v == a.i8v, "8-bit mismatch");
    CHECK(b.u16v == a.u16v && b.i16v == a.i16v, "16-bit mismatch");
    CHECK(b.u32v == a.u32v && b.i32v == a.i32v, "32-bit mismatch");
    CHECK(b.u64v == a.u64v && b.i64v == a.i64v, "64-bit mismatch");
    CHECK(b.blob_len == 4 && memcmp(b.blob, a.blob, 4) == 0, "bytes mismatch");
    CHECK(b.name_len == 8 && memcmp(b.name, a.name, 8) == 0, "string mismatch");
    PASS();
}

static void
test_zero_copy(void)
{
    TEST("bytes and string decode into the source buffer");

    uint8_t buf[64];
    struct everything a = {
        .blob = (const uint8_t *)"xy", .blob_len = 2,
        .name = "z", .name_len = 1,
    };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    CHECK(n > 0 && everything_decode(&b, buf, n) > 0, "codec failed");
    CHECK((const uint8_t *)b.blob >= buf &&
          (const uint8_t *)b.blob < buf + n, "bytes not a view into buf");
    CHECK((const uint8_t *)b.name >= buf &&
          (const uint8_t *)b.name < buf + n, "string not a view into buf");
    PASS();
}

static void
test_empty_fields(void)
{
    TEST("zero-length bytes and string round-trip");

    uint8_t buf[64];
    struct everything a = { .blob_len = 0, .name_len = 0 };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    CHECK(n > 0 && everything_decode(&b, buf, n) > 0, "codec failed");
    CHECK(b.blob_len == 0 && b.name_len == 0, "lengths not zero");
    PASS();
}

static void
test_union_variants(void)
{
    TEST("each union variant round-trips");

    uint8_t buf[64];
    struct event out, in;
    int n;

    struct event ping = { .at = 100, .kind = KIND_PING, .seq = 42 };
    n = event_encode(&ping, buf, sizeof(buf));
    CHECK(n > 0 && event_decode(&in, buf, n) > 0, "Ping codec failed");
    CHECK(in.kind == KIND_PING && in.seq == 42 && in.at == 100,
          "Ping fields wrong");

    struct event data = {
        .at = 200, .kind = KIND_DATA, .length = 3,
        .payload = (const uint8_t *)"abc", .payload_len = 3,
    };
    n = event_encode(&data, buf, sizeof(buf));
    CHECK(n > 0 && event_decode(&in, buf, n) > 0, "Data codec failed");
    CHECK(in.kind == KIND_DATA && in.length == 3 && in.payload_len == 3,
          "Data fields wrong");

    out = (struct event){ .at = 300, .kind = KIND_BYE, .reason = 7 };
    n = event_encode(&out, buf, sizeof(buf));
    CHECK(n > 0 && event_decode(&in, buf, n) > 0, "Bye codec failed");
    CHECK(in.kind == KIND_BYE && in.reason == 7, "Bye fields wrong");
    PASS();
}

static void
test_union_unset_fields_zero(void)
{
    TEST("fields of other variants decode to zero");

    uint8_t buf[64];
    struct event ping = { .at = 1, .kind = KIND_PING, .seq = 9 };
    struct event in;
    int n = event_encode(&ping, buf, sizeof(buf));

    CHECK(n > 0 && event_decode(&in, buf, n) > 0, "codec failed");
    /* Only the Ping variant was written, so Data and Bye fields are zero. */
    CHECK(in.length == 0 && in.payload_len == 0 && in.reason == 0,
          "an unwritten variant field was not zero");
    PASS();
}

static void
test_max_field_number(void)
{
    TEST("a field at number 31 round-trips");

    uint8_t buf[16];
    struct edge a = { .top = 0x01020304 };
    struct edge b;
    int n = edge_encode(&a, buf, sizeof(buf));

    CHECK(n > 0 && edge_decode(&b, buf, n) > 0, "codec failed");
    CHECK(b.top == a.top, "value mismatch");
    PASS();
}

static void
test_write_overflow(void)
{
    TEST("a short output buffer returns -1, not an overrun");

    struct everything a = {
        .name = "this will not fit", .name_len = 17,
    };
    uint8_t small[8];

    CHECK(everything_encode(&a, small, sizeof(small)) == -1,
          "encode did not report overflow");
    PASS();
}

static void
test_read_truncated(void)
{
    TEST("a truncated input returns -1");

    uint8_t buf[256];
    struct everything a = {
        .blob = (const uint8_t *)"\x01\x02\x03\x04", .blob_len = 4,
        .name = "microser", .name_len = 8,
    };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    CHECK(n > 4, "setup encode failed");
    /* The 2-byte length prefix says the body is longer than we now supply. */
    CHECK(everything_decode(&b, buf, n - 3) == -1,
          "decode accepted a truncated body");
    PASS();
}

static void
test_forward_compat_skip(void)
{
    TEST("a reader skips a field it does not know");

    /*
     * Hand-build an Everything message that also carries a field number the
     * struct has no member for, once for each wire type, as a newer writer
     * would.  The decoder must step over each and still recover the fields it
     * does know.
     */
    uint8_t buf[128];
    int pos = 2;

    pos = ms_write_tag_u32(buf, pos, sizeof(buf), 5, 0xcafef00d);   /* u32v */
    pos = ms_write_tag_u8(buf, pos, sizeof(buf), 15, 0x11);         /* unknown 8 */
    pos = ms_write_tag_u16(buf, pos, sizeof(buf), 16, 0x2222);      /* unknown 16 */
    pos = ms_write_tag_u32(buf, pos, sizeof(buf), 17, 0x33333333);  /* unknown 32 */
    pos = ms_write_tag_u64(buf, pos, sizeof(buf), 18, 0x44ULL);     /* unknown 64 */
    pos = ms_write_tag_bytes(buf, pos, sizeof(buf), 19, "skip", 4); /* unknown bytes */
    pos = ms_write_tag_u8(buf, pos, sizeof(buf), 1, 0x77);          /* u8v */
    CHECK(pos > 0, "hand-build overran");

    buf[0] = (uint8_t)((pos - 2) & 0xff);
    buf[1] = (uint8_t)(((pos - 2) >> 8) & 0xff);

    struct everything b;
    CHECK(everything_decode(&b, buf, pos) == pos, "decode failed on skips");
    CHECK(b.u32v == 0xcafef00d, "known u32 field lost across skips");
    CHECK(b.u8v == 0x77, "known u8 field after the skips was lost");
    PASS();
}

int
main(void)
{
    printf("microser tests:\n");

    test_every_scalar();
    test_zero_copy();
    test_empty_fields();
    test_union_variants();
    test_union_unset_fields_zero();
    test_max_field_number();
    test_write_overflow();
    test_read_truncated();
    test_forward_compat_skip();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

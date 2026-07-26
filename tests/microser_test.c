/* microser_test.c : round-trip and edge tests for the microser codecs */

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

    TEST("every scalar type round-trips");
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
    uint8_t buf[64];
    struct everything a = {
        .blob = (const uint8_t *)"xy", .blob_len = 2,
        .name = "z", .name_len = 1,
    };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    TEST("bytes and string decode into the source buffer");
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
    uint8_t buf[64];
    struct everything a = { .blob_len = 0, .name_len = 0 };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    TEST("zero-length bytes and string round-trip");
    CHECK(n > 0 && everything_decode(&b, buf, n) > 0, "codec failed");
    CHECK(b.blob_len == 0 && b.name_len == 0, "lengths not zero");
    PASS();
}

static void
test_union_variants(void)
{
    uint8_t buf[64];
    struct event out, in;
    int n;

    struct event ping = { .at = 100, .kind = KIND_PING, .seq = 42 };
    struct event data = {
        .at = 200, .kind = KIND_DATA, .length = 3,
        .payload = (const uint8_t *)"abc", .payload_len = 3,
    };

    TEST("each union variant round-trips");
    n = event_encode(&ping, buf, sizeof(buf));
    CHECK(n > 0 && event_decode(&in, buf, n) > 0, "Ping codec failed");
    CHECK(in.kind == KIND_PING && in.seq == 42 && in.at == 100,
          "Ping fields wrong");

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
    uint8_t buf[64];
    struct event ping = { .at = 1, .kind = KIND_PING, .seq = 9 };
    struct event in;
    int n = event_encode(&ping, buf, sizeof(buf));

    TEST("fields of other variants decode to zero");
    CHECK(n > 0 && event_decode(&in, buf, n) > 0, "codec failed");
    /* Only the Ping variant was written, so Data and Bye fields are zero. */
    CHECK(in.length == 0 && in.payload_len == 0 && in.reason == 0,
          "an unwritten variant field was not zero");
    PASS();
}

static void
test_max_field_number(void)
{
    uint8_t buf[16];
    struct edge a = { .top = 0x01020304 };
    struct edge b;
    int n = edge_encode(&a, buf, sizeof(buf));

    TEST("a field at number 31 round-trips");
    CHECK(n > 0 && edge_decode(&b, buf, n) > 0, "codec failed");
    CHECK(b.top == a.top, "value mismatch");
    PASS();
}

static void
test_write_overflow(void)
{
    struct everything a = {
        .name = "this will not fit", .name_len = 17,
    };
    uint8_t small[8];

    TEST("a short output buffer returns -1, not an overrun");
    CHECK(everything_encode(&a, small, sizeof(small)) == -1,
          "encode did not report overflow");
    PASS();
}

static void
test_read_truncated(void)
{
    uint8_t buf[256];
    struct everything a = {
        .blob = (const uint8_t *)"\x01\x02\x03\x04", .blob_len = 4,
        .name = "microser", .name_len = 8,
    };
    struct everything b;
    int n = everything_encode(&a, buf, sizeof(buf));

    TEST("a truncated input returns -1");
    CHECK(n > 4, "setup encode failed");
    /* The 2-byte length prefix says the body is longer than we now supply. */
    CHECK(everything_decode(&b, buf, n - 3) == -1,
          "decode accepted a truncated body");
    PASS();
}

static void
test_forward_compat_skip(void)
{
    uint8_t buf[128];
    struct everything b;
    int pos = 2;

    TEST("a reader skips a field it does not know");

    /*
     * Hand-build an Everything message that also carries a field number the
     * struct has no member for, once for each wire type, as a newer writer
     * would.  The decoder must step over each and still recover the fields it
     * does know.
     */
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

    CHECK(everything_decode(&b, buf, pos) == pos, "decode failed on skips");
    CHECK(b.u32v == 0xcafef00d, "known u32 field lost across skips");
    CHECK(b.u8v == 0x77, "known u8 field after the skips was lost");
    PASS();
}

static void
test_dispatch_round_trip(void)
{
    uint8_t buf[128];
    struct proto_msg m;
    int n;

    struct edge e = { .top = 0xabcd1234 };
    struct event ev = { .at = 5, .kind = KIND_PING, .seq = 99 };

    TEST("dispatch encodes a tag and decodes the right variant");
    n = proto_encode_edge(buf, sizeof(buf), &e);
    CHECK(n > 0, "encode failed");
    CHECK(buf[0] == PROTO_EDGE, "wrong tag byte on the wire");
    CHECK(proto_msg_type(buf, n) == PROTO_EDGE, "msg_type read the wrong tag");
    CHECK(proto_decode(buf, n, &m) == n, "decode did not consume the frame");
    CHECK(m.type == PROTO_EDGE, "decoded the wrong type");
    CHECK(m.u.edge.top == e.top, "payload lost through dispatch");

    n = proto_encode_event(buf, sizeof(buf), &ev);
    CHECK(n > 0 && proto_decode(buf, n, &m) == n, "Event through dispatch");
    CHECK(m.type == PROTO_EVENT && m.u.event.seq == 99, "Event payload wrong");
    PASS();
}

static void
test_dispatch_unknown_tag(void)
{
    /* A tag this build has no case for, over an empty (2-byte) body. A newer
     * peer sending a message type we predate must not wedge the reader. */
    uint8_t frame[3] = { 200, 0x00, 0x00 };
    struct proto_msg m;

    TEST("an unknown message type is skipped, not an error");

    CHECK(proto_decode(frame, sizeof(frame), &m) == 3,
          "unknown message not consumed by its length prefix");
    CHECK(m.type == PROTO_NONE, "unknown tag did not report PROTO_NONE");

    /* A tag with no body at all is malformed. */
    CHECK(proto_decode(frame, 1, &m) == -1, "a bodiless frame was accepted");
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
    test_dispatch_round_trip();
    test_dispatch_unknown_tag();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

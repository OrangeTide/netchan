/* nc_ws_test.c : known-answer + round-trip checks for the WebSocket codec */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "nc_ws.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

int
main(void)
{
    /* RFC 6455 section 1.3 known-answer handshake. */
    const char *req =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    char resp[256];
    int n = nc_ws_accept(req, strlen(req), resp, sizeof(resp));
    CHECK(n > 0);
    CHECK(strstr(resp, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
    CHECK(strncmp(resp, "HTTP/1.1 101", 12) == 0);

    /* Incomplete request -> "need more". */
    CHECK(nc_ws_accept("GET /x HTTP/1.1\r\nUpgrade: web", 28, resp, sizeof(resp)) == 0);

    /* Complete but non-WebSocket request -> reject. */
    const char *plain = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";
    CHECK(nc_ws_accept(plain, strlen(plain), resp, sizeof(resp)) == -1);

    /* Client request builds a well-formed upgrade; its expected accept value
     * matches what the server side computes for the same key. */
    uint8_t key16[16];
    for (int i = 0; i < 16; i++) key16[i] = (uint8_t)(i * 7 + 1);
    char creq[256], expect[32];
    size_t cl = nc_ws_client_request(creq, sizeof(creq), "host:9099", "/ws",
                                     key16, expect);
    CHECK(cl > 0);
    char sresp[256];
    int sn = nc_ws_accept(creq, cl, sresp, sizeof(sresp));
    CHECK(sn > 0);
    CHECK(nc_ws_client_verify(sresp, (size_t)sn, expect) == 1);

    /* Frame round-trip: client builds masked, server parses and unmasks. */
    const char *msg = "netchan datagram bytes";
    uint8_t mask4[4] = { 0x12, 0x34, 0x56, 0x78 };
    uint8_t frame[128];
    size_t fl = nc_ws_frame_build(frame, sizeof(frame), NC_WS_BINARY,
                                  msg, strlen(msg), mask4);
    CHECK(fl > 0);
    CHECK((frame[1] & 0x80) != 0);          /* mask bit set */
    struct nc_ws_frame f;
    long used = nc_ws_frame_parse(frame, fl, &f);
    CHECK(used == (long)fl);
    CHECK(f.opcode == NC_WS_BINARY);
    CHECK(f.fin == 1);
    CHECK(f.payload_len == strlen(msg));
    CHECK(memcmp(f.payload, msg, strlen(msg)) == 0);

    /* Partial frame -> parser reports "need more" without consuming. */
    CHECK(nc_ws_frame_parse(frame, fl - 1, &f) == 0);

    /* Extended 16-bit length path (payload >= 126). */
    uint8_t big[400];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (uint8_t)(i * 3);
    uint8_t bframe[512];
    size_t bl = nc_ws_frame_build(bframe, sizeof(bframe), NC_WS_BINARY,
                                  big, sizeof(big), NULL);   /* server: no mask */
    CHECK(bl == sizeof(big) + 4);
    CHECK((bframe[1] & 0x7f) == 126);
    long bused = nc_ws_frame_parse(bframe, bl, &f);
    CHECK(bused == (long)bl);
    CHECK(f.payload_len == sizeof(big));
    CHECK(memcmp(f.payload, big, sizeof(big)) == 0);

    if (fails == 0)
        printf("nc_ws_test: all checks passed\n");
    return fails ? 1 : 0;
}

/* nc_ws.h : dependency-free WebSocket framing and handshake for netchan */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef NC_WS_H
#define NC_WS_H

#include <stddef.h>
#include <stdint.h>

/*
 * A small, self-contained WebSocket (RFC 6455) codec: just enough to carry
 * netchan datagrams over a browser-reachable pipe. One binary WebSocket
 * message holds exactly one netchan packet, so the datagram boundary the
 * protocol core relies on survives the trip.
 *
 * The whole file is transport plumbing. It knows nothing about netchan; it
 * turns a byte stream into framed messages and back. Both ends of the demo
 * use it: the gateway drives the server side (accepts the upgrade, reads
 * masked client frames, writes unmasked frames) and the native test client
 * drives the client side (sends the upgrade, masks its frames, verifies the
 * server's reply). The browser gets the same behaviour for free from its
 * built-in WebSocket object.
 *
 * SHA-1 and base64, needed only for the handshake, are bundled here so the
 * codec pulls in no external library.
 */

/* Frame opcodes (the 4-bit field in a frame header). */
#define NC_WS_CONT    0x0
#define NC_WS_TEXT    0x1
#define NC_WS_BINARY  0x2
#define NC_WS_CLOSE   0x8
#define NC_WS_PING    0x9
#define NC_WS_PONG    0xA

/* A parsed frame. payload points into the caller's buffer (unmasked in
 * place), so that buffer must stay put and writable while the frame is used. */
struct nc_ws_frame {
    int      opcode;
    int      fin;
    uint8_t *payload;
    size_t   payload_len;
};

/*
 * Server handshake. Inspect an accumulated HTTP request in req[0..req_len).
 *   >0  a complete, valid Upgrade request: the 101 response is written into
 *       resp (up to resp_cap) and its length returned.
 *    0  the request headers are not complete yet (read more, then retry).
 *   -1  headers are complete but this is not a valid WebSocket upgrade
 *       (the caller may treat it as a plain HTTP request).
 */
int nc_ws_accept(const char *req, size_t req_len, char *resp, size_t resp_cap);

/*
 * Client handshake. Build a GET upgrade request for host/path using the
 * caller-supplied 16 random key bytes, and write the Sec-WebSocket-Accept
 * value the server must echo into expect (needs 32 bytes). Returns the
 * request length written into buf, or 0 if it does not fit.
 */
size_t nc_ws_client_request(char *buf, size_t cap, const char *host,
                            const char *path, const uint8_t key16[16],
                            char expect[32]);

/*
 * Verify a server's handshake reply in resp[0..len).
 *   1  complete and the accept value matches expect: connected.
 *   0  the response headers are not complete yet.
 *  -1  complete but rejected or the accept value is wrong.
 */
int nc_ws_client_verify(const char *resp, size_t len, const char *expect);

/*
 * Parse one frame from the front of buf[0..len). buf must be writable: a
 * masked payload is unmasked in place. Returns the number of bytes the frame
 * occupies (so the caller can slide the buffer down), 0 if a full frame is
 * not present yet, or -1 on a protocol error. On success *f describes the
 * frame and f->payload points into buf.
 */
long nc_ws_frame_parse(uint8_t *buf, size_t len, struct nc_ws_frame *f);

/*
 * Build a single FIN frame carrying payload with the given opcode into
 * out[0..cap). If mask4 is non-NULL the payload is masked with those four
 * bytes (clients must mask; servers must not). Returns the total frame
 * length, or 0 if it does not fit.
 */
size_t nc_ws_frame_build(uint8_t *out, size_t cap, int opcode,
                         const void *payload, size_t payload_len,
                         const uint8_t mask4[4]);

#endif /* NC_WS_H */

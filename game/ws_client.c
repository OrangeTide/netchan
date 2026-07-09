/* ws_client.c : headless Caves-of-Thor client over netchan/WebSocket */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The same headless client as game_client.c, but reaching the server through
 * the WebSocket gateway instead of a UDP socket. It is the browser's path
 * exercised from C: it opens a WebSocket, sends and receives netchan
 * datagrams as binary messages, joins the game, reproduces the map from the
 * seed, drives an input, and applies snapshots. Because it links the same
 * netchan core as the native client, a green run here means a browser sitting
 * behind the gateway plays on the same server as the terminal client.
 *
 * The transport address handed to netchan is a fixed one-byte handle: over a
 * WebSocket there is exactly one peer (the gateway), so the core never needs
 * to tell peers apart by address. That is the nc_addr seam doing its job.
 *
 *   ws_client [host] [ws_port] [run_ms] [move_dir 0..7]
 */

#include "netchan.h"
#include "nc_ws.h"
#include "proto.h"
#include "game.h"
#include "game_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define POLL_MS 15

/* over a WebSocket there is a single peer; any non-empty handle will do */
static const struct nc_addr GATE = { 1, { 1 } };

static uint32_t
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* tiny non-cryptographic PRNG: the WebSocket mask only defeats caching
 * proxies, it is not a security boundary. */
static uint32_t rng_state;
static uint32_t
xrand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Connect a blocking TCP socket to host:port. Returns fd or -1. */
static int
tcp_connect(const char *host, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Send a netchan datagram to the gateway as one masked binary message. */
static void
ws_send_dgram(int fd, const uint8_t *data, size_t len)
{
    uint8_t mask[4];
    uint32_t r = xrand();
    memcpy(mask, &r, 4);
    uint8_t frame[2048 + 16];
    size_t fl = nc_ws_frame_build(frame, sizeof(frame), NC_WS_BINARY,
                                  data, len, mask);
    if (fl == 0)
        return;
    size_t off = 0;
    while (off < fl) {
        ssize_t n = send(fd, frame + off, fl - off, 0);
        if (n <= 0)
            return;
        off += (size_t)n;
    }
}

/* Flush every packet netchan wants to send out over the WebSocket. */
static void
flush(int fd, struct netchan_conn *c)
{
    uint8_t buf[2048];
    struct nc_addr dst;
    for (;;) {
        size_t n = netchan_send_next(c, buf, sizeof(buf), &dst);
        if (n == 0)
            break;
        ws_send_dgram(fd, buf, n);
    }
}

int
main(int argc, char **argv)
{
    const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t port    = (argc > 2) ? (uint16_t)atoi(argv[2]) : 8080;
    uint32_t run_ms  = (argc > 3) ? (uint32_t)atoi(argv[3]) : 2500;
    uint8_t move_dir = (argc > 4) ? (uint8_t)atoi(argv[4]) : 2;   /* east */

    rng_state = (now_ms() ^ ((uint32_t)getpid() << 16)) | 1u;

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        fprintf(stderr, "ws_client: cannot connect to %s:%u\n", host, port);
        return 1;
    }

    /* WebSocket upgrade handshake. */
    uint8_t key16[16];
    for (int i = 0; i < 16; i++)
        key16[i] = (uint8_t)xrand();
    char req[256], expect[32];
    size_t rl = nc_ws_client_request(req, sizeof(req), host, "/game",
                                     key16, expect);
    if (send(fd, req, rl, 0) != (ssize_t)rl) {
        fprintf(stderr, "ws_client: handshake send failed\n");
        return 1;
    }
    uint8_t rx[8192];
    size_t rxn = 0;
    int handshaken = 0;
    uint32_t hstart = now_ms();
    while (!handshaken && now_ms() - hstart < 2000) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, POLL_MS) <= 0)
            continue;
        ssize_t n = recv(fd, rx + rxn, sizeof(rx) - rxn, 0);
        if (n <= 0)
            break;
        rxn += (size_t)n;
        int v = nc_ws_client_verify((const char *)rx, rxn, expect);
        if (v == 1) {
            handshaken = 1;
            /* discard the response header block; the body starts after it */
            for (size_t i = 0; i + 3 < rxn; i++)
                if (!memcmp(rx + i, "\r\n\r\n", 4)) {
                    memmove(rx, rx + i + 4, rxn - (i + 4));
                    rxn -= (i + 4);
                    break;
                }
        } else if (v < 0) {
            fprintf(stderr, "ws_client: handshake rejected\n");
            return 1;
        }
    }
    if (!handshaken) {
        fprintf(stderr, "ws_client: handshake timed out\n");
        return 1;
    }

    struct netchan_conn *c = netchan_open(0);
    netchan_connect(c, &GATE);

    struct netchan_chan *in_ch = NULL, *wel_ch = NULL, *snap_ch = NULL;
    struct world w;
    memset(&w, 0, sizeof(w));
    int my_player = -1, have_map = 0, snaps = 0;
    uint32_t seq = 0, last_input = 0, start = now_ms();

    while (now_ms() - start < run_ms) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        poll(&pfd, 1, POLL_MS);

        if (pfd.revents & POLLIN) {
            ssize_t n = recv(fd, rx + rxn, sizeof(rx) - rxn, 0);
            if (n <= 0)
                break;              /* gateway closed */
            rxn += (size_t)n;
            for (;;) {
                struct nc_ws_frame fr;
                long used = nc_ws_frame_parse(rx, rxn, &fr);
                if (used <= 0)
                    break;
                if (fr.opcode == NC_WS_BINARY)
                    netchan_feed(c, fr.payload, fr.payload_len, &GATE);
                memmove(rx, rx + used, rxn - (size_t)used);
                rxn -= (size_t)used;
            }
        }

        netchan_service(c, now_ms());

        if (!in_ch && netchan_state(c) == NETCHAN_STATE_CONNECTED)
            in_ch = netchan_chan_open(c, NETCHAN_UNRELIABLE, NETCHAN_DIR_SEND, "input");

        struct netchan_event ev;
        while (netchan_poll(c, &ev)) {
            if (ev.type == NETCHAN_EV_CHAN_OPEN) {
                if (netchan_chan_type(ev.ch) == NETCHAN_RELIABLE)
                    wel_ch = ev.ch;
                else
                    snap_ch = ev.ch;
            } else if (ev.type == NETCHAN_EV_DATA) {
                uint8_t buf[512];
                int rd = netchan_chan_read(ev.ch, buf, sizeof(buf));
                if (rd <= 0)
                    continue;
                if (ev.ch == wel_ch) {
                    struct welcome wl;
                    if (welcome_decode(&wl, buf, rd) > 0) {
                        my_player = wl.your_player;
                        game_init(&w, wl.seed);
                        have_map = 1;
                        printf("ws_client: welcome, player %d, seed %u\n",
                               my_player, wl.seed);
                    }
                } else if (ev.ch == snap_ch) {
                    struct snapshot sn;
                    if (snapshot_decode(&sn, buf, rd) > 0 && have_map) {
                        gw_unpack(&w, sn.state, sn.state_len);
                        w.tick = (uint16_t)sn.tick;
                        snaps++;
                    }
                }
            }
        }

        if (in_ch && netchan_chan_state(in_ch) == 1 && now_ms() - last_input >= 100) {
            last_input = now_ms();
            struct player_input pi = { .seq = ++seq,
                                       .input = IN_MAKE(move_dir, IN_DIR_NONE) };
            uint8_t buf[32];
            int m = player_input_encode(&pi, buf, sizeof(buf));
            if (m > 0)
                netchan_chan_write(in_ch, buf, m);
        }

        flush(fd, c);
    }

    printf("ws_client: state=%d, player=%d, snapshots=%d\n",
           netchan_state(c), my_player, snaps);
    if (my_player >= 0 && have_map) {
        struct player *me = &w.players[my_player];
        printf("ws_client: my player at (%u,%u) hp=%u alive=%u, tick=%u\n",
               me->x, me->y, me->hp, me->alive, w.tick);
    }

    netchan_close(c);
    close(fd);
    return (my_player >= 0 && snaps > 0) ? 0 : 1;
}

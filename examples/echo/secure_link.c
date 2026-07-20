/* secure_link.c : encrypted netchan session driven by the iox event loop */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "secure_link.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_timer.h"
#include "netchan.h"
#include "nc_udp.h"

/* How often the retransmit/keepalive service runs while the loop is idle.
 * netchan's own timers decide what actually goes on the wire; this is just
 * the coarse tick that lets them fire. */
#define SL_TICK_MS 50

struct secure_link {
    struct iox_loop     *loop;
    struct netchan_conn *conn;
    struct netchan_chan *tx;        /* our reliable send channel */
    struct netchan_chan *rx;        /* the peer's channel, learned on open */
    int                  fd;
    int                  server;
    int                  encrypted;
    int                  have_peer;
    int                  connect_sent;  /* client: netchan_connect issued */
    int                  accepted;      /* server: netchan_accept issued */
    int                  up;            /* on_up already fired */
    int                  closing;       /* teardown started; stop the tick */
    int                  timer_id;
    struct nc_addr       peer;
    struct nc_crypto     crypto;
    struct secure_link_cb cb;
};

/* CLOCK_MONOTONIC in milliseconds, matching netchan's internal clock. */
static uint32_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Send one unsealed nc_crypto HELLO to the pinned peer. */
static void
send_hello(struct secure_link *sl)
{
    uint8_t hp[NC_CRYPTO_HELLO_LEN];
    struct sockaddr_storage ss;
    socklen_t slen;
    size_t hn;

    if (!sl->have_peer)
        return;
    hn = nc_crypto_handshake_packet(&sl->crypto, hp, sizeof(hp));
    slen = nc_udp_to_sockaddr(&sl->peer, &ss);
    if (hn == 0 || slen == 0)
        return;
    (void)sendto(sl->fd, hp, hn, 0, (struct sockaddr *)&ss, slen);
}

/* Drain every datagram netchan has queued, sealing each when encrypted. */
static void
flush_out(struct secure_link *sl)
{
    uint8_t buf[2048];
    uint8_t sealed[2048 + NC_CRYPTO_OVERHEAD];
    struct nc_addr to;
    struct sockaddr_storage ss;
    socklen_t slen;
    size_t m;

    while ((m = netchan_send_next(sl->conn, buf, sizeof(buf), &to)) != 0) {
        const uint8_t *out = buf;
        size_t outlen = m;

        if (sl->encrypted) {
            long sn = nc_crypto_seal(&sl->crypto, buf, m,
                                     sealed, sizeof(sealed));
            if (sn < 0)
                continue;       /* keys not ready; netchan resends later */
            out = sealed;
            outlen = (size_t)sn;
        }
        slen = nc_udp_to_sockaddr(&to, &ss);
        if (slen == 0)
            continue;
        (void)sendto(sl->fd, out, outlen, 0, (struct sockaddr *)&ss, slen);
    }
}

/* Advance the two nested handshakes and open the send channel once the
 * connection reaches CONNECTED. Safe to call after any event. */
static void
advance(struct secure_link *sl)
{
    /* Client: the crypto session must be ready before we speak netchan, so
     * the connect SYN itself is sealed. */
    if (!sl->server && !sl->connect_sent) {
        if (!sl->encrypted || nc_crypto_ready(&sl->crypto)) {
            if (netchan_connect(sl->conn, &sl->peer) == NETCHAN_OK)
                sl->connect_sent = 1;
        }
    }

    if (!sl->up && sl->tx == NULL &&
        netchan_state(sl->conn) == NETCHAN_STATE_CONNECTED) {
        sl->tx = netchan_chan_open(sl->conn, NETCHAN_RELIABLE,
                                   NETCHAN_DIR_SEND, "echo");
        if (sl->tx) {
            sl->up = 1;
            if (sl->cb.on_up)
                sl->cb.on_up(sl, sl->cb.user);
        }
    }
}

/* Drain netchan's event queue: track the peer's channel, deliver received
 * bytes, and notice disconnects. */
static void
pump_events(struct secure_link *sl)
{
    struct netchan_event ev;
    uint8_t buf[2048];

    while (netchan_poll(sl->conn, &ev)) {
        switch (ev.type) {
        case NETCHAN_EV_CHAN_OPEN:
        case NETCHAN_EV_DATA:
            if (ev.ch)
                sl->rx = ev.ch;
            break;
        case NETCHAN_EV_DISCONNECTED:
            if (sl->cb.on_down)
                sl->cb.on_down(sl, sl->cb.user);
            break;
        default:
            break;
        }
    }

    if (sl->rx && sl->cb.on_data) {
        int rd;

        while ((rd = netchan_chan_read(sl->rx, buf, sizeof(buf))) > 0)
            sl->cb.on_data(sl, buf, (size_t)rd, sl->cb.user);
    }
}

/* One inbound datagram: crypto-open, feed netchan, accept on the server's
 * first data packet. */
static void
on_readable(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    struct secure_link *sl = arg;
    uint8_t buf[2048];
    uint8_t plain[2048];
    struct sockaddr_storage ss;
    struct nc_addr from;
    socklen_t slen;
    ssize_t r;

    (void)loop;
    (void)events;

    for (;;) {
        const uint8_t *data;
        size_t dlen;

        slen = sizeof(ss);
        r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&ss, &slen);
        if (r <= 0)
            break;      /* EAGAIN drains the socket for this wakeup */

        if (nc_udp_from_sockaddr(&from, (struct sockaddr *)&ss, slen) != 0)
            continue;

        /* The server pins its peer from the first datagram so it can answer
         * the crypto handshake before netchan is accepted. */
        if (sl->server && !sl->have_peer) {
            sl->peer = from;
            sl->have_peer = 1;
        }

        data = buf;
        dlen = (size_t)r;
        if (sl->encrypted) {
            int was_hello = (buf[0] == NC_CRYPTO_HELLO);
            long pn = nc_crypto_open(&sl->crypto, buf, (size_t)r,
                                     plain, sizeof(plain));

            /* The responder answers each HELLO until the peer switches to
             * sealed DATA, covering a lost handshake reply. */
            if (was_hello && sl->server)
                send_hello(sl);
            if (pn <= 0)
                continue;   /* HELLO consumed, replay, or bad auth */
            data = plain;
            dlen = (size_t)pn;
        }

        if (sl->server && !sl->accepted) {
            netchan_feed(sl->conn, data, dlen, &from);
            netchan_accept(sl->conn);
            sl->accepted = 1;
        } else {
            netchan_feed(sl->conn, data, dlen, &from);
        }
    }

    pump_events(sl);
    advance(sl);
    flush_out(sl);
}

/* Periodic tick: fire netchan's retransmit timers, repeat the client HELLO
 * until the session is ready, and flush. Reschedules itself. */
static void
on_tick(struct iox_loop *loop, void *arg)
{
    struct secure_link *sl = arg;

    if (sl->encrypted && !sl->server && !nc_crypto_ready(&sl->crypto))
        send_hello(sl);

    netchan_service(sl->conn, now_ms());
    /* Drain here too, not only after a datagram. netchan_service is what
     * raises the idle-timeout disconnect, and a peer that has gone silent
     * sends nothing to trigger the other drain. */
    pump_events(sl);
    advance(sl);
    flush_out(sl);

    /*
     * Keep the tick alive until this link is torn down. Asking the loop
     * whether it is running is the wrong question: iox reports "stopped"
     * before iox_loop_run has ever been called, and secure_link_open kicks
     * the first tick from exactly that moment. Gating on it retired the timer
     * at birth, leaving the session with no clock at all: no retransmits, no
     * repeated HELLO, and no idle timeout.
     */
    if (!sl->closing)
        sl->timer_id = iox_timer_add(loop, SL_TICK_MS, on_tick, sl);
    else
        sl->timer_id = -1;
}

struct secure_link *
secure_link_open(struct iox_loop *loop, int fd, int server,
                 const struct nc_addr *peer, const uint8_t *psk,
                 int use_crypto, const struct secure_link_cb *cb)
{
    struct secure_link *sl;

    if (!server && !peer)
        return NULL;

    sl = calloc(1, sizeof(*sl));
    if (!sl)
        return NULL;

    sl->conn = netchan_open(server ? 1 : 0);
    if (!sl->conn) {
        free(sl);
        return NULL;
    }
    sl->loop = loop;
    sl->timer_id = -1;      /* calloc leaves 0, which is a valid timer id */
    sl->fd = fd;
    sl->server = server ? 1 : 0;
    if (peer) {
        sl->peer = *peer;
        sl->have_peer = 1;
    }
    if (cb)
        sl->cb = *cb;

    if (use_crypto) {
        /* role: 0 = initiator (client), 1 = responder (server). No static
         * identity key here: this example is secrecy without authentication,
         * which is the NN handshake. See examples/auth for the other half. */
        struct nc_crypto_cfg ccfg = { .psk = psk };
        if (nc_crypto_init(&sl->crypto, sl->server, &ccfg) != 0) {
            netchan_close(sl->conn);
            free(sl);
            return NULL;
        }
        sl->encrypted = 1;
    }

    if (iox_fd_add(loop, fd, IOX_READ, on_readable, sl) != 0) {
        netchan_close(sl->conn);
        free(sl);
        return NULL;
    }

    /* Kick the handshakes immediately, then let the timer keep them going. */
    on_tick(loop, sl);
    return sl;
}

int
secure_link_send(struct secure_link *sl, const void *data, size_t len)
{
    int spins;

    if (!sl->up || !sl->tx)
        return -1;

    for (spins = 0; spins < 64; spins++) {
        int w = netchan_chan_write(sl->tx, data, len);

        if (w == (int)len) {
            flush_out(sl);
            return 0;
        }
        if (w != NETCHAN_ERR_FLOW && w != NETCHAN_ERR_AGAIN)
            return -1;
        /* Window full: service to reap acks, then retry. */
        netchan_service(sl->conn, now_ms());
        flush_out(sl);
    }
    return -1;
}

int
secure_link_up(const struct secure_link *sl)
{
    return sl->up;
}

void
secure_link_close(struct secure_link *sl)
{
    if (!sl)
        return;
    sl->closing = 1;
    if (sl->timer_id >= 0)
        iox_timer_remove(sl->loop, sl->timer_id);
    iox_fd_remove(sl->loop, sl->fd);
    if (sl->conn)
        netchan_close(sl->conn);
    free(sl);
}

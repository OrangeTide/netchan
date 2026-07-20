/* auth_link.c : authenticated encrypted netchan session on the iox loop */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "auth_link.h"

#include <stdio.h>
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
#define AL_TICK_MS 50

/*
 * One reliable channel carries two conversations, told apart by a leading
 * byte. netchan preserves message boundaries on a reliable channel, so a tag
 * is all the framing this needs, and the login does not have to negotiate a
 * second channel before it can say anything.
 */
#define TAG_AUTH 0xa0
#define TAG_APP  0xa1

struct auth_link {
    struct iox_loop     *loop;
    struct netchan_conn *conn;
    struct netchan_chan *tx;        /* our reliable send channel */
    struct netchan_chan *rx;        /* the peer's channel, learned on open */
    int                  fd;
    int                  server;
    int                  have_peer;
    int                  connect_sent;  /* client: netchan_connect issued */
    int                  accepted;      /* server: netchan_accept issued */
    int                  auth_started;
    int                  need_fired;    /* which need we last reported */
    int                  up;            /* on_up already fired */
    int                  down;          /* on_down already fired */
    int                  closing;       /* teardown started; stop the tick */
    int                  timer_id;
    struct nc_addr       peer;
    struct nc_crypto     crypto;
    struct nc_auth       auth;
    struct auth_link_cb  cb;
    struct nc_auth_server_cb scb;
    char                 user[NC_AUTH_MAX_USER + 1];
};

/* CLOCK_MONOTONIC in milliseconds, matching netchan's internal clock. */
static uint32_t
now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
fire_down(struct auth_link *al, int reason)
{
    if (al->down)
        return;
    al->down = 1;
    if (al->cb.on_down)
        al->cb.on_down(al, reason, al->cb.user);
}

/* Send one unsealed nc_crypto HELLO to the pinned peer. */
static void
send_hello(struct auth_link *al)
{
    uint8_t hp[NC_CRYPTO_HELLO_LEN];
    struct sockaddr_storage ss;
    socklen_t slen;
    size_t hn;

    if (!al->have_peer)
        return;
    hn = nc_crypto_handshake_packet(&al->crypto, hp, sizeof(hp));
    slen = nc_udp_to_sockaddr(&al->peer, &ss);
    if (hn == 0 || slen == 0)
        return;
    (void)sendto(al->fd, hp, hn, 0, (struct sockaddr *)&ss, slen);
}

/* Drain every datagram netchan has queued, sealing each one. */
static void
flush_out(struct auth_link *al)
{
    uint8_t buf[2048];
    uint8_t sealed[2048 + NC_CRYPTO_OVERHEAD];
    struct nc_addr to;
    struct sockaddr_storage ss;
    socklen_t slen;
    size_t m;

    while ((m = netchan_send_next(al->conn, buf, sizeof(buf), &to)) != 0) {
        long sn = nc_crypto_seal(&al->crypto, buf, m, sealed, sizeof(sealed));

        if (sn < 0)
            continue;       /* keys not ready; netchan resends later */
        slen = nc_udp_to_sockaddr(&to, &ss);
        if (slen == 0)
            continue;
        (void)sendto(al->fd, sealed, (size_t)sn, 0,
                     (struct sockaddr *)&ss, slen);
    }
}

/* Put one tagged message on the reliable channel, servicing the connection
 * if the send window is momentarily full. */
static int
chan_send(struct auth_link *al, uint8_t tag, const void *data, size_t len)
{
    uint8_t msg[2048];
    int spins;

    if (!al->tx || len + 1 > sizeof(msg))
        return -1;
    msg[0] = tag;
    memcpy(msg + 1, data, len);

    for (spins = 0; spins < 64; spins++) {
        int w = netchan_chan_write(al->tx, msg, len + 1);

        if (w == (int)(len + 1)) {
            flush_out(al);
            return 0;
        }
        if (w != NETCHAN_ERR_FLOW && w != NETCHAN_ERR_AGAIN)
            return -1;
        netchan_service(al->conn, now_ms());
        flush_out(al);
    }
    return -1;
}

static void
auth_out(void *ctx, const void *msg, size_t len)
{
    chan_send(ctx, TAG_AUTH, msg, len);
}

/*
 * Report a request for a credential once per transition. The handler may
 * answer now or in ten seconds; nothing here waits on it, and the loop keeps
 * turning so netchan's timers go on firing while the user types.
 */
static void
settle_need(struct auth_link *al)
{
    int need;

    if (!al->auth_started || al->server || al->up || al->down)
        return;

    need = nc_auth_needs(&al->auth);
    if (need == al->need_fired)
        return;
    al->need_fired = need;
    if (need != NC_AUTH_NEED_NOTHING && al->cb.on_need)
        al->cb.on_need(al, need, al->cb.user);
}

/* The login has resolved one way or the other; report it once. */
static void
settle_auth(struct auth_link *al)
{
    if (!al->auth_started || al->up || al->down)
        return;

    settle_need(al);

    if (nc_auth_state(&al->auth) == NC_AUTH_OK) {
        snprintf(al->user, sizeof(al->user), "%s", nc_auth_user(&al->auth));
        al->up = 1;
        if (al->cb.on_up)
            al->cb.on_up(al, al->cb.user);
    } else if (nc_auth_state(&al->auth) == NC_AUTH_DENIED) {
        fire_down(al, AL_DOWN_AUTH);
    }
}

/*
 * Advance the stack. Each stage waits on the one below it: netchan does not
 * connect until the crypto session is ready, and the login does not start
 * until there is a reliable channel to carry it.
 */
static void
advance(struct auth_link *al)
{
    if (nc_crypto_failed(&al->crypto)) {
        fire_down(al, AL_DOWN_HOSTKEY);
        return;
    }

    if (!al->server && !al->connect_sent && nc_crypto_ready(&al->crypto)) {
        if (netchan_connect(al->conn, &al->peer) == NETCHAN_OK)
            al->connect_sent = 1;
    }

    if (al->tx == NULL &&
        netchan_state(al->conn) == NETCHAN_STATE_CONNECTED) {
        al->tx = netchan_chan_open(al->conn, NETCHAN_RELIABLE,
                                   NETCHAN_DIR_SEND, "auth");
    }

    if (al->tx && !al->auth_started) {
        const uint8_t *sid = nc_crypto_session_id(&al->crypto);

        if (sid) {
            if (al->server)
                nc_auth_server_init(&al->auth, sid, &al->scb, auth_out, al);
            else
                nc_auth_client_init(&al->auth, sid, al->user, auth_out, al);
            al->auth_started = 1;
            if (!al->server)
                nc_auth_start(&al->auth);
        }
    }

    settle_auth(al);
}

/* One message off the reliable channel: a login step, or application bytes
 * that are only delivered once the login has succeeded. */
static void
deliver(struct auth_link *al, const uint8_t *msg, size_t len)
{
    if (len < 1)
        return;

    if (msg[0] == TAG_AUTH) {
        if (al->auth_started)
            nc_auth_feed(&al->auth, msg + 1, len - 1);
        settle_auth(al);
        return;
    }

    if (msg[0] == TAG_APP && al->up && al->cb.on_data)
        al->cb.on_data(al, msg + 1, len - 1, al->cb.user);
}

/* Drain netchan's event queue: track the peer's channel, deliver received
 * messages, and notice disconnects. */
static void
pump_events(struct auth_link *al)
{
    struct netchan_event ev;
    uint8_t buf[2048];

    while (netchan_poll(al->conn, &ev)) {
        switch (ev.type) {
        case NETCHAN_EV_CHAN_OPEN:
        case NETCHAN_EV_DATA:
            if (ev.ch)
                al->rx = ev.ch;
            break;
        case NETCHAN_EV_DISCONNECTED:
            fire_down(al, AL_DOWN_PEER);
            break;
        default:
            break;
        }
    }

    if (al->rx) {
        int rd;

        while ((rd = netchan_chan_read(al->rx, buf, sizeof(buf))) > 0)
            deliver(al, buf, (size_t)rd);
    }
}

/* One inbound datagram: crypto-open, feed netchan, accept on the server's
 * first data packet. */
static void
on_readable(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    struct auth_link *al = arg;
    uint8_t buf[2048];
    uint8_t plain[2048];
    struct sockaddr_storage ss;
    struct nc_addr from;
    socklen_t slen;
    ssize_t r;

    (void)loop;
    (void)events;

    for (;;) {
        int was_hello;
        long pn;

        slen = sizeof(ss);
        r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&ss, &slen);
        if (r <= 0)
            break;      /* EAGAIN drains the socket for this wakeup */

        if (nc_udp_from_sockaddr(&from, (struct sockaddr *)&ss, slen) != 0)
            continue;

        /* The server pins its peer from the first datagram so it can answer
         * the crypto handshake before netchan is accepted. */
        if (al->server && !al->have_peer) {
            al->peer = from;
            al->have_peer = 1;
        }

        was_hello = (buf[0] == NC_CRYPTO_HELLO);
        pn = nc_crypto_open(&al->crypto, buf, (size_t)r, plain, sizeof(plain));

        if (nc_crypto_failed(&al->crypto))
            break;      /* the peer's identity key was refused */

        /* The responder answers each HELLO until the peer switches to
         * sealed DATA, covering a lost handshake reply. */
        if (was_hello && al->server)
            send_hello(al);
        if (pn <= 0)
            continue;   /* HELLO consumed, replay, or bad auth */

        if (al->server && !al->accepted) {
            netchan_feed(al->conn, plain, (size_t)pn, &from);
            netchan_accept(al->conn);
            al->accepted = 1;
        } else {
            netchan_feed(al->conn, plain, (size_t)pn, &from);
        }
    }

    pump_events(al);
    advance(al);
    flush_out(al);
}

/* Periodic tick: fire netchan's retransmit timers, repeat the client HELLO
 * until the session is ready, and flush. Reschedules itself. */
static void
on_tick(struct iox_loop *loop, void *arg)
{
    struct auth_link *al = arg;

    if (!al->server && !nc_crypto_ready(&al->crypto))
        send_hello(al);

    netchan_service(al->conn, now_ms());
    /* Drain here too, not only after a datagram. netchan_service is what
     * raises the idle-timeout disconnect, and a peer that has gone silent
     * sends nothing to trigger the other drain, so the event would sit in
     * the queue forever and the dead session would look healthy. */
    pump_events(al);
    advance(al);
    flush_out(al);

    /*
     * Keep the tick alive until this link is torn down. Asking the loop
     * whether it is running is the wrong question: iox reports "stopped"
     * before iox_loop_run has ever been called, and auth_link_open kicks the
     * first tick from exactly that moment. Gating on it silently retired the
     * timer at birth, leaving the session with no clock at all: no
     * retransmits, no repeated HELLO, and no idle timeout, with everything
     * limping along on whatever the peer happened to send.
     */
    if (!al->closing)
        al->timer_id = iox_timer_add(loop, AL_TICK_MS, on_tick, al);
    else
        al->timer_id = -1;
}

struct auth_link *
auth_link_open(struct iox_loop *loop, int fd, const struct auth_link_cfg *cfg,
               const struct auth_link_cb *cb)
{
    struct auth_link *al;
    struct nc_crypto_cfg ccfg;

    if (!cfg || (!cfg->server && !cfg->peer))
        return NULL;

    al = calloc(1, sizeof(*al));
    if (!al)
        return NULL;

    al->conn = netchan_open(cfg->server ? 1 : 0);
    if (!al->conn) {
        free(al);
        return NULL;
    }
    al->loop = loop;
    al->fd = fd;
    al->server = cfg->server ? 1 : 0;
    al->timer_id = -1;
    al->scb = cfg->scb;
    if (cfg->peer) {
        al->peer = *cfg->peer;
        al->have_peer = 1;
    }
    if (cfg->user)
        snprintf(al->user, sizeof(al->user), "%s", cfg->user);
    if (cb)
        al->cb = *cb;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.psk = cfg->psk;
    ccfg.static_sk = cfg->static_sk;
    ccfg.require_peer_static = cfg->require_peer_static;
    ccfg.verify_peer = cfg->verify_peer;
    ccfg.verify_ctx = cfg->verify_ctx;

    /* role: 0 = initiator (client), 1 = responder (server). */
    if (nc_crypto_init(&al->crypto, al->server, &ccfg) != 0) {
        netchan_close(al->conn);
        free(al);
        return NULL;
    }

    if (iox_fd_add(loop, fd, IOX_READ, on_readable, al) != 0) {
        netchan_close(al->conn);
        free(al);
        return NULL;
    }

    /* Kick the handshakes immediately, then let the timer keep them going. */
    on_tick(loop, al);
    return al;
}

void
auth_link_supply_key(struct auth_link *al, const uint8_t *sk, const uint8_t *pk)
{
    if (!al->auth_started || al->server)
        return;
    nc_auth_supply_key(&al->auth, sk, pk);
    al->need_fired = NC_AUTH_NEED_NOTHING;
    settle_auth(al);
    flush_out(al);
}

void
auth_link_supply_password(struct auth_link *al, const char *password)
{
    if (!al->auth_started || al->server)
        return;
    nc_auth_supply_password(&al->auth, password);
    al->need_fired = NC_AUTH_NEED_NOTHING;
    settle_auth(al);
    flush_out(al);
}

int
auth_link_send(struct auth_link *al, const void *data, size_t len)
{
    if (!al->up)
        return -1;
    return chan_send(al, TAG_APP, data, len);
}

int
auth_link_up(const struct auth_link *al)
{
    return al->up;
}

const char *
auth_link_user(const struct auth_link *al)
{
    return al->user;
}

void
auth_link_close(struct auth_link *al)
{
    if (!al)
        return;
    al->closing = 1;
    if (al->timer_id >= 0)
        iox_timer_remove(al->loop, al->timer_id);
    iox_fd_remove(al->loop, al->fd);
    if (al->conn)
        netchan_close(al->conn);
    free(al);
}

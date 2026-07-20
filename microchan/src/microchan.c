/* microchan.c : transport-agnostic core (handshake, mux, reliability) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "microchan.h"
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Internal constants
 ****************************************************************/

#define MC_HDR 8            /* packet header bytes                       */
#define MC_REC 6            /* per-record header bytes                   */

/* packet types (header byte 4) */
enum {
    PKT_SYN = 1,
    PKT_SYNACK,
    PKT_ACK,                /* handshake completion ack                  */
    PKT_DATA,
    PKT_FIN,
};

/* record reliability flag (record byte 1) */
#define MT_UNREL 0
#define MT_REL   1

/* timers, in ms */
#define RT_MS            120    /* reliable retransmit base               */
#define RT_MAX_MS       1000    /* reliable retransmit cap                */
#define RT_ATTEMPTS       12    /* give up after this many resends        */
#define CONNECT_RETRY_MS 250
#define CONNECT_RETRIES  240    /* keep trying ~1 min before giving up     */
#define KEEPALIVE_MS    1000
#define IDLE_MS        10000

#define MC_EVENTQ 8

/****************************************************************
 * Byte I/O helpers (big-endian on the wire)
 ****************************************************************/

static void
wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void
wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t
rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t
rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

/* 16-bit serial-number arithmetic (RFC 1982): is a strictly after b? */
static int
seq_after(uint16_t a, uint16_t b)
{
    return (int16_t)(uint16_t)(a - b) > 0;
}

/****************************************************************
 * Types
 ****************************************************************/

struct mc_msg {
    uint16_t len;
    uint8_t  data[MC_MAXMSG];
};

struct mc_chan {
    struct microchan *conn;
    uint8_t used;
    uint8_t type;
    uint8_t id;
    /* received-datagram delivery ring (both channel types) */
    struct mc_msg rxq[MC_RECVQ];
    uint8_t rx_head;
    uint8_t rx_count;
};

struct tx_slot {
    uint16_t seq;
    uint16_t len;
    uint8_t  chan;
    uint8_t  data[MC_MAXMSG];
};

struct microchan {
    uint8_t  is_server;
    uint8_t  state;
    uint32_t local_id;
    uint32_t remote_id;
    struct mc_addr peer;

    uint32_t now;
    uint32_t last_recv;
    uint32_t last_send;

    /* handshake */
    uint8_t  send_syn;
    uint8_t  send_synack;
    uint8_t  send_ack;
    uint32_t connect_timer;
    uint16_t connect_attempts;

    /* reliable transmit window (shared by all reliable channels) */
    struct tx_slot txw[MC_WINDOW];
    uint16_t tx_base;       /* oldest unacked seq                        */
    uint16_t tx_next;       /* next seq to assign                        */
    uint16_t tx_unsent;     /* next seq not yet put on the wire          */
    uint32_t tx_timer;      /* retransmit deadline, 0 = nothing pending  */
    uint16_t tx_attempts;

    /* reliable receive (connection-global next expected seq) */
    uint16_t rx_next;
    uint8_t  ack_pending;

    /* unreliable transmit ring */
    struct mc_msg txu[MC_UNREL_TXQ];
    uint8_t  txu_chan[MC_UNREL_TXQ];
    uint8_t  txu_head;
    uint8_t  txu_count;

    struct mc_chan chan[MC_MAX_CHAN];

    struct mc_event evq[MC_EVENTQ];
    uint8_t ev_head;
    uint8_t ev_count;
};

/****************************************************************
 * Event queue
 ****************************************************************/

static void
ev_push(struct microchan *c, int type, struct mc_chan *ch)
{
    struct mc_event *e;
    if (c->ev_count >= MC_EVENTQ)
        return;                 /* best effort; data stays readable      */
    e = &c->evq[(c->ev_head + c->ev_count) % MC_EVENTQ];
    memset(e, 0, sizeof(*e));
    e->type = type;
    e->ch = ch;
    c->ev_count++;
}

int
mc_poll(struct microchan *c, struct mc_event *ev)
{
    if (!c || c->ev_count == 0)
        return 0;
    *ev = c->evq[c->ev_head];
    c->ev_head = (uint8_t)((c->ev_head + 1) % MC_EVENTQ);
    c->ev_count--;
    return 1;
}

/****************************************************************
 * Connection lifecycle
 ****************************************************************/

static uint32_t
gen_id(void)
{
    uint32_t id = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
    return id ? id : 1;         /* 0 is reserved for new-connection      */
}

struct microchan *
mc_open(int is_server)
{
    struct microchan *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->is_server = (uint8_t)(is_server ? 1 : 0);
    c->state = MC_STATE_CLOSED;
    c->local_id = gen_id();
    return c;
}

void
mc_close(struct microchan *c)
{
    free(c);
}

int
mc_state(struct microchan *c)
{
    return c ? c->state : MC_STATE_CLOSED;
}

uint32_t
mc_id(struct microchan *c)
{
    return c ? c->local_id : 0;
}

int
mc_connect(struct microchan *c, const struct mc_addr *addr)
{
    if (!c || !addr)
        return MC_ERR;
    c->peer = *addr;
    c->state = MC_STATE_CONNECTING;
    c->send_syn = 1;
    c->connect_timer = c->now + CONNECT_RETRY_MS;
    c->connect_attempts = 0;
    return MC_OK;
}

uint32_t
mc_peek_id(const void *pkt, size_t len)
{
    if (len < MC_HDR)
        return 0;
    return rd32((const uint8_t *)pkt);
}

/****************************************************************
 * Channels
 ****************************************************************/

struct mc_chan *
mc_chan_open(struct microchan *c, int type)
{
    int i;
    if (!c)
        return NULL;
    for (i = 0; i < MC_MAX_CHAN; i++) {
        if (!c->chan[i].used) {
            struct mc_chan *ch = &c->chan[i];
            memset(ch, 0, sizeof(*ch));
            ch->used = 1;
            ch->conn = c;
            ch->type = (uint8_t)type;
            ch->id = (uint8_t)i;
            return ch;
        }
    }
    return NULL;
}

void
mc_chan_close(struct mc_chan *ch)
{
    if (ch)
        ch->used = 0;
}

int
mc_chan_id(struct mc_chan *ch)
{
    return ch ? ch->id : -1;
}

static void
rxq_push(struct mc_chan *ch, const uint8_t *data, uint16_t len)
{
    struct mc_msg *m;
    if (ch->rx_count >= MC_RECVQ)
        return;
    m = &ch->rxq[(ch->rx_head + ch->rx_count) % MC_RECVQ];
    if (len > MC_MAXMSG)
        len = MC_MAXMSG;
    m->len = len;
    memcpy(m->data, data, len);
    ch->rx_count++;
}

static int
rxq_full(struct mc_chan *ch)
{
    return ch->rx_count >= MC_RECVQ;
}

int
mc_read(struct mc_chan *ch, void *buf, size_t buflen)
{
    struct mc_msg *m;
    if (!ch || ch->rx_count == 0)
        return 0;
    m = &ch->rxq[ch->rx_head];
    if (m->len > buflen)
        return MC_ERR_TOOBIG;
    memcpy(buf, m->data, m->len);
    ch->rx_head = (uint8_t)((ch->rx_head + 1) % MC_RECVQ);
    ch->rx_count--;
    return (int)m->len;
}

int
mc_write(struct mc_chan *ch, const void *data, size_t len)
{
    struct microchan *c;
    if (!ch || !ch->used)
        return MC_ERR;
    c = ch->conn;
    if (c->state != MC_STATE_CONNECTED)
        return MC_ERR_CLOSED;
    if (len > MC_MAXMSG)
        return MC_ERR_TOOBIG;

    if (ch->type == MC_RELIABLE) {
        struct tx_slot *s;
        if ((uint16_t)(c->tx_next - c->tx_base) >= MC_WINDOW)
            return MC_ERR_AGAIN;
        s = &c->txw[c->tx_next % MC_WINDOW];
        s->seq = c->tx_next;
        s->chan = ch->id;
        s->len = (uint16_t)len;
        memcpy(s->data, data, len);
        c->tx_next++;
    } else {
        struct mc_msg *m;
        if (c->txu_count >= MC_UNREL_TXQ) {
            /* drop the oldest queued unreliable datagram */
            c->txu_head = (uint8_t)((c->txu_head + 1) % MC_UNREL_TXQ);
            c->txu_count--;
        }
        m = &c->txu[(c->txu_head + c->txu_count) % MC_UNREL_TXQ];
        c->txu_chan[(c->txu_head + c->txu_count) % MC_UNREL_TXQ] = ch->id;
        m->len = (uint16_t)len;
        memcpy(m->data, data, len);
        c->txu_count++;
    }
    return (int)len;
}

/****************************************************************
 * Receive path
 ****************************************************************/

static struct mc_chan *
chan_by_id(struct microchan *c, uint8_t id)
{
    if (id < MC_MAX_CHAN && c->chan[id].used)
        return &c->chan[id];
    return NULL;
}

static void
process_ack(struct microchan *c, uint16_t ack)
{
    int progressed = 0;
    while (c->tx_base != c->tx_next && seq_after(ack, c->tx_base)) {
        c->tx_base++;
        progressed = 1;
    }
    if (seq_after(c->tx_base, c->tx_unsent))
        c->tx_unsent = c->tx_base;
    if (progressed) {
        c->tx_attempts = 0;
        c->tx_timer = (c->tx_base == c->tx_next) ? 0 : c->now + RT_MS;
    }
}

static void
deliver(struct microchan *c, uint8_t chan, uint8_t mt, uint16_t seq,
        const uint8_t *data, uint16_t len)
{
    struct mc_chan *ch = chan_by_id(c, chan);

    if (mt == MT_REL) {
        if (seq == c->rx_next) {
            if (ch) {
                if (rxq_full(ch)) {
                    /* no room: do not advance, force a retransmit later */
                    c->ack_pending = 1;
                    return;
                }
                rxq_push(ch, data, len);
                ev_push(c, MC_EV_DATA, ch);
            }
            c->rx_next++;
        }
        /* future or duplicate: drop, but ack so the sender resyncs */
        c->ack_pending = 1;
    } else {
        if (ch) {
            rxq_push(ch, data, len);
            ev_push(c, MC_EV_DATA, ch);
        }
    }
}

int
mc_feed(struct microchan *c, const void *pkt, size_t len,
        const struct mc_addr *from)
{
    const uint8_t *p = (const uint8_t *)pkt;
    uint32_t conn_id;
    uint8_t ptype;
    uint16_t ack;

    if (!c || len < MC_HDR)
        return MC_ERR_PROTO;

    conn_id = rd32(p);
    ptype = p[4];
    ack = rd16(p + 6);
    c->last_recv = c->now;

    switch (ptype) {
    case PKT_SYN:
        if (!c->is_server)
            return MC_ERR_PROTO;
        if (len < MC_HDR + 4)
            return MC_ERR_PROTO;
        if (c->state == MC_STATE_CLOSED) {
            if (from)
                c->peer = *from;
            c->remote_id = rd32(p + MC_HDR);
            c->state = MC_STATE_CONNECTING;
            c->connect_timer = c->now + CONNECT_RETRY_MS;
            c->connect_attempts = 0;
        }
        if (c->state == MC_STATE_CONNECTING)
            c->send_synack = 1; /* (re)send accept until the peer is up */
        break;

    case PKT_SYNACK:
        if (c->is_server || conn_id != c->local_id)
            return MC_ERR_PROTO;
        if (len < MC_HDR + 4)
            return MC_ERR_PROTO;
        if (from)
            c->peer = *from;    /* lock onto the server's real address;
                                 * the SYN may have gone to a broadcast */
        c->remote_id = rd32(p + MC_HDR);
        if (c->state == MC_STATE_CONNECTING) {
            c->state = MC_STATE_CONNECTED;
            ev_push(c, MC_EV_CONNECTED, NULL);
        }
        c->send_ack = 1;
        break;

    case PKT_ACK:
        if (!c->is_server || conn_id != c->local_id)
            return MC_ERR_PROTO;
        if (c->state == MC_STATE_CONNECTING) {
            c->state = MC_STATE_CONNECTED;
            ev_push(c, MC_EV_CONNECTED, NULL);
        }
        break;

    case PKT_DATA: {
        const uint8_t *q = p + MC_HDR;
        const uint8_t *end = p + len;
        if (conn_id != c->local_id)
            return MC_ERR_PROTO;
        if (c->is_server && c->state == MC_STATE_CONNECTING) {
            c->state = MC_STATE_CONNECTED;   /* ACK was lost; data implies up */
            ev_push(c, MC_EV_CONNECTED, NULL);
        }
        if (c->state != MC_STATE_CONNECTED)
            return MC_OK;
        process_ack(c, ack);
        while (q + MC_REC <= end) {
            uint8_t chan = q[0];
            uint8_t mt = q[1];
            uint16_t seq = rd16(q + 2);
            uint16_t dlen = rd16(q + 4);
            q += MC_REC;
            if (q + dlen > end)
                break;
            deliver(c, chan, mt, seq, q, dlen);
            q += dlen;
        }
        break;
    }

    case PKT_FIN:
        if (conn_id != c->local_id)
            return MC_ERR_PROTO;
        if (c->state != MC_STATE_CLOSED) {
            c->state = MC_STATE_CLOSED;
            ev_push(c, MC_EV_DISCONNECTED, NULL);
        }
        break;

    default:
        return MC_ERR_PROTO;
    }
    return MC_OK;
}

/****************************************************************
 * Transmit path
 ****************************************************************/

size_t
mc_send_next(struct microchan *c, void *buf, size_t buflen, struct mc_addr *to)
{
    uint8_t *b = (uint8_t *)buf;
    uint8_t *w;
    size_t cap;

    if (!c || buflen < MC_HDR)
        return 0;
    cap = buflen < MC_MTU ? buflen : MC_MTU;

    /* control packets take priority */
    if (c->send_synack) {
        wr32(b, c->remote_id);
        b[4] = PKT_SYNACK;
        b[5] = 0;
        wr16(b + 6, 0);
        wr32(b + MC_HDR, c->local_id);
        c->send_synack = 0;
        c->last_send = c->now;
        if (to) *to = c->peer;
        return MC_HDR + 4;
    }
    if (c->send_ack) {
        wr32(b, c->remote_id);
        b[4] = PKT_ACK;
        b[5] = 0;
        wr16(b + 6, 0);
        c->send_ack = 0;
        c->last_send = c->now;
        if (to) *to = c->peer;
        return MC_HDR;
    }
    if (c->send_syn) {
        wr32(b, 0);
        b[4] = PKT_SYN;
        b[5] = 0;
        wr16(b + 6, 0);
        wr32(b + MC_HDR, c->local_id);
        c->send_syn = 0;        /* re-armed by mc_service on retry */
        c->last_send = c->now;
        if (to) *to = c->peer;
        return MC_HDR + 4;
    }

    if (c->state != MC_STATE_CONNECTED)
        return 0;

    /* nothing to say? */
    if (c->tx_unsent == c->tx_next && c->txu_count == 0 && !c->ack_pending)
        return 0;

    w = b + MC_HDR;

    /* reliable records (only those not yet on the wire) */
    while (c->tx_unsent != c->tx_next) {
        struct tx_slot *s = &c->txw[c->tx_unsent % MC_WINDOW];
        size_t need = MC_REC + s->len;
        if ((size_t)(w - b) + need > cap)
            break;
        w[0] = s->chan;
        w[1] = MT_REL;
        wr16(w + 2, s->seq);
        wr16(w + 4, s->len);
        memcpy(w + MC_REC, s->data, s->len);
        w += need;
        c->tx_unsent++;
    }
    if (c->tx_base != c->tx_next && c->tx_timer == 0)
        c->tx_timer = c->now + RT_MS;

    /* unreliable records */
    while (c->txu_count > 0) {
        struct mc_msg *m = &c->txu[c->txu_head];
        size_t need = MC_REC + m->len;
        if ((size_t)(w - b) + need > cap)
            break;
        w[0] = c->txu_chan[c->txu_head];
        w[1] = MT_UNREL;
        wr16(w + 2, 0);
        wr16(w + 4, m->len);
        memcpy(w + MC_REC, m->data, m->len);
        w += need;
        c->txu_head = (uint8_t)((c->txu_head + 1) % MC_UNREL_TXQ);
        c->txu_count--;
    }

    wr32(b, c->remote_id);
    b[4] = PKT_DATA;
    b[5] = 0;
    wr16(b + 6, c->rx_next);
    c->ack_pending = 0;
    c->last_send = c->now;
    if (to) *to = c->peer;
    return (size_t)(w - b);
}

/****************************************************************
 * Timers
 ****************************************************************/

int
mc_service(struct microchan *c, uint32_t now_ms)
{
    if (!c)
        return -1;
    c->now = now_ms;

    if (c->state == MC_STATE_CONNECTING) {
        if (now_ms >= c->connect_timer) {
            c->connect_attempts++;
            if (c->connect_attempts > CONNECT_RETRIES) {
                c->state = MC_STATE_CLOSED;
                ev_push(c, MC_EV_DISCONNECTED, NULL);
                return -1;
            }
            if (c->is_server)
                c->send_synack = 1;
            else
                c->send_syn = 1;
            c->connect_timer = now_ms + CONNECT_RETRY_MS;
        }
        return (int)CONNECT_RETRY_MS;
    }

    if (c->state == MC_STATE_CONNECTED) {
        if (now_ms - c->last_recv > IDLE_MS) {
            c->state = MC_STATE_CLOSED;
            ev_push(c, MC_EV_DISCONNECTED, NULL);
            return -1;
        }
        if (c->tx_base != c->tx_next && c->tx_timer &&
            now_ms >= c->tx_timer) {
            uint32_t backoff;
            c->tx_attempts++;
            if (c->tx_attempts > RT_ATTEMPTS) {
                c->state = MC_STATE_CLOSED;
                ev_push(c, MC_EV_DISCONNECTED, NULL);
                return -1;
            }
            c->tx_unsent = c->tx_base;       /* resend the whole window */
            backoff = (uint32_t)RT_MS << (c->tx_attempts < 4 ?
                                          c->tx_attempts : 3);
            if (backoff > RT_MAX_MS)
                backoff = RT_MAX_MS;
            c->tx_timer = now_ms + backoff;
        }
        if (now_ms - c->last_send > KEEPALIVE_MS)
            c->ack_pending = 1;              /* emit an empty DATA keepalive */
        return (int)RT_MS;
    }

    return -1;
}

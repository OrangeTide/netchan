/* microchan.h : multiplexed reliable/unreliable channels over IPX or UDP */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef MICROCHAN_H
#define MICROCHAN_H

#include <stdint.h>
#include <stddef.h>
#include "mc_addr.h"

/****************************************************************
 * Compile-time tunables
 *
 * Override with -D on the command line to shrink the static
 * footprint for memory-constrained targets (e.g. 16-bit DOS).
 * The defaults are sized for a 4-player game.
 ****************************************************************/

#ifndef MC_MTU
#define MC_MTU 546          /* IPX media-independent payload guarantee   */
#endif
#ifndef MC_WINDOW
#define MC_WINDOW 8         /* reliable send window, in messages         */
#endif
#ifndef MC_MAX_CHAN
#define MC_MAX_CHAN 2       /* mux channels per connection               */
#endif
#ifndef MC_RECVQ
#define MC_RECVQ MC_WINDOW  /* per-channel recv slots; must hold a full
                             * reliable window or delivery can stall      */
#endif
#ifndef MC_UNREL_TXQ
#define MC_UNREL_TXQ 4      /* queued unreliable datagrams awaiting send */
#endif

/* Suggested peer-pool size for a server. The core is one connection per
 * peer; the application keeps an array of this many and routes with
 * mc_peek_id(). Not used by the core itself. */
#ifndef MC_MAX_CONN
#define MC_MAX_CONN 4
#endif

/* Largest application datagram: one packet header + one record header. */
#define MC_MAXMSG (MC_MTU - 8 - 6)

/****************************************************************
 * Constants
 ****************************************************************/

enum {
    MC_RELIABLE,            /* ordered, acked, retransmitted datagrams   */
    MC_UNRELIABLE,          /* fire-and-forget datagrams                 */
};

enum {
    MC_STATE_CLOSED,
    MC_STATE_CONNECTING,
    MC_STATE_CONNECTED,
    MC_STATE_CLOSING,
};

enum {
    MC_EV_NONE,
    MC_EV_CONNECTED,
    MC_EV_DISCONNECTED,
    MC_EV_DATA,             /* a channel has a datagram ready to read    */
    MC_EV_REDIRECT,         /* lobby handed us off to a game host        */
};

enum {
    MC_OK         =  0,
    MC_ERR        = -1,
    MC_ERR_NOMEM  = -2,
    MC_ERR_AGAIN  = -3,
    MC_ERR_CLOSED = -4,
    MC_ERR_TOOBIG = -5,
    MC_ERR_PROTO  = -6,
};

/****************************************************************
 * Types
 ****************************************************************/

struct microchan;
struct mc_chan;

struct mc_event {
    int type;
    struct mc_chan *ch;     /* channel for MC_EV_DATA                    */
    struct mc_addr redirect_addr;
    uint32_t redirect_id;
};

/****************************************************************
 * Connection lifecycle
 *
 * The application owns the transport. It feeds received packets in
 * with mc_feed() and pulls packets to transmit out with mc_send_next().
 * This keeps the core transport-agnostic: IPX on DOS, UDP on a host.
 *
 * A connection is one peer. A server keeps an array of connections and
 * routes inbound datagrams with mc_peek_id(): a header id of 0 is a new
 * connection attempt (feed it to a fresh server connection), otherwise
 * route to the connection whose id matches.
 ****************************************************************/

struct microchan *mc_open(int is_server);
void mc_close(struct microchan *c);
int mc_state(struct microchan *c);
uint32_t mc_id(struct microchan *c);

/** Begin a client handshake toward addr. */
int mc_connect(struct microchan *c, const struct mc_addr *addr);

/** Read the connection id from a raw datagram for server-side routing.
 *  Returns 0 for a new-connection (SYN) datagram. */
uint32_t mc_peek_id(const void *pkt, size_t len);

/** Feed one received datagram (from address) into the connection. */
int mc_feed(struct microchan *c, const void *pkt, size_t len,
            const struct mc_addr *from);

/** Fill buf with the next packet to transmit and its destination.
 *  Returns byte count, or 0 when nothing is pending. */
size_t mc_send_next(struct microchan *c, void *buf, size_t buflen,
                    struct mc_addr *to);

/** Service timers (retransmit, keepalive, timeout). now_ms is a free
 *  running millisecond clock. Returns ms until the next call is needed,
 *  or -1 when idle. */
int mc_service(struct microchan *c, uint32_t now_ms);

/****************************************************************
 * Channels
 *
 * Both peers must open the same channel layout in the same order: the
 * channel id is the open order (0, 1, ...). There is no negotiation.
 ****************************************************************/

struct mc_chan *mc_chan_open(struct microchan *c, int type);
void mc_chan_close(struct mc_chan *ch);
int mc_chan_id(struct mc_chan *ch);

/** Queue one datagram for sending. Returns bytes queued or an MC_ERR_*. */
int mc_write(struct mc_chan *ch, const void *data, size_t len);

/** Read the next received datagram. Returns bytes read, 0 if none. */
int mc_read(struct mc_chan *ch, void *buf, size_t buflen);

/****************************************************************
 * Events -- drain after mc_feed()
 ****************************************************************/

int mc_poll(struct microchan *c, struct mc_event *ev);

#endif /* MICROCHAN_H */

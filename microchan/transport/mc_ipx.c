/* mc_ipx.c : 16-bit MS-DOS IPX transport for microchan (polling, no ESR) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "mc_ipx.h"
#include <dos.h>
#include <i86.h>
#include <string.h>

/****************************************************************
 * IPX far-call entry point and thin register thunks
 *
 * The driver is reached by an indirect far call through mc_ipx_entry with
 * the function number in BX. Each thunk hardcodes BX (letting Watcom load
 * it via a parm register would clobber it while ES:SI is being set up) and
 * brackets the call with push/pop of BP and DS: real-mode IPX only promises
 * to preserve CS, DS, SS, and the compiler assumes BP and DS survive a call.
 ****************************************************************/

void (__far *mc_ipx_entry)(void);

/* Listen For Packet (BX=4): ES:SI -> ECB. */
extern void ipx_listen(void __far *ecb);
#pragma aux ipx_listen =                \
    "push bp" "push ds"                 \
    "mov bx, 4"                         \
    "call dword ptr [mc_ipx_entry]"     \
    "pop ds" "pop bp"                   \
    parm [es si] modify [ax bx cx dx si di es];

/* Send Packet (BX=3): ES:SI -> ECB. */
extern void ipx_send_pkt(void __far *ecb);
#pragma aux ipx_send_pkt =              \
    "push bp" "push ds"                 \
    "mov bx, 3"                         \
    "call dword ptr [mc_ipx_entry]"     \
    "pop ds" "pop bp"                   \
    parm [es si] modify [ax bx cx dx si di es];

/* Open Socket (BX=0): DX = socket, AL = longevity (0xFF = until closed). */
extern unsigned char ipx_open_socket(unsigned dx_socket);
#pragma aux ipx_open_socket =           \
    "push bp" "push ds"                 \
    "xor bx, bx" "mov al, 0FFh"         \
    "call dword ptr [mc_ipx_entry]"     \
    "pop ds" "pop bp"                   \
    parm [dx] value [al] modify [ax bx cx dx si di es];

/* Close Socket (BX=1): DX = socket. */
extern void ipx_close_socket(unsigned dx_socket);
#pragma aux ipx_close_socket =          \
    "push bp" "push ds"                 \
    "mov bx, 1"                         \
    "call dword ptr [mc_ipx_entry]"     \
    "pop ds" "pop bp"                   \
    parm [dx] modify [ax bx cx dx si di es];

/* Get Internetwork Address (BX=9): ES:SI -> 10-byte net(4)+node(6). */
extern void ipx_get_addr(void __far *buf);
#pragma aux ipx_get_addr =              \
    "push bp" "push ds"                 \
    "mov bx, 9"                         \
    "call dword ptr [mc_ipx_entry]"     \
    "pop ds" "pop bp"                   \
    parm [es si] modify [ax bx cx dx si di es];

/****************************************************************
 * On-the-wire structures (real-mode, byte-packed)
 ****************************************************************/

#pragma pack(push, 1)

struct ipx_ecb {
    void __far *link;           /* 0  */
    void __far *esr;            /* 4  : 0:0 for polling */
    uint8_t  in_use;            /* 8  */
    uint8_t  ccode;             /* 9  : 0xFF pending, 0 done, else error */
    uint8_t  socket[2];         /* 10 : big-endian */
    uint8_t  ipx_work[4];       /* 12 */
    uint8_t  drv_work[12];      /* 16 */
    uint8_t  imm_addr[6];       /* 28 : immediate (next-hop) node */
    uint16_t frag_count;        /* 34 */
    void __far *frag_addr;      /* 36 */
    uint16_t frag_size;         /* 40 */
};                              /* 42 */

struct ipx_hdr {
    uint8_t  checksum[2];       /* 0  : 0xFFFF */
    uint8_t  length[2];         /* 2  : big-endian, header + data */
    uint8_t  tc;                /* 4  : transport control (hops) */
    uint8_t  ptype;             /* 5  : packet type */
    uint8_t  dst_net[4];        /* 6  */
    uint8_t  dst_node[6];       /* 10 */
    uint8_t  dst_sock[2];       /* 16 : big-endian */
    uint8_t  src_net[4];        /* 18 */
    uint8_t  src_node[6];       /* 22 */
    uint8_t  src_sock[2];       /* 28 : big-endian */
};                              /* 30 */

#pragma pack(pop)

#define IPX_HDR_LEN 30
#define IPX_BUFSZ   (IPX_HDR_LEN + MC_MTU)
#define CC_PENDING  0xFF

/****************************************************************
 * Static pools
 *
 * ECBs and packet buffers live in static (BSS) storage, not the C heap and
 * not _dos_allocmem: a Watcom large-model program grabs all conventional
 * memory at startup, so _dos_allocmem has nothing to give and would hand
 * back an overlapping segment. Static far arrays sit in conventional RAM,
 * each comfortably inside one 64 KB segment, which is exactly what the
 * real-mode driver needs. This is also the project's whole point: no
 * runtime allocation, no fragmentation, a fixed worst case fixed at link.
 ****************************************************************/

static struct ipx_ecb ipx_recv_ecb[MC_IPX_RECV];
static struct ipx_ecb ipx_send_ecb[MC_IPX_SEND];
static uint8_t ipx_recv_buf[MC_IPX_RECV][IPX_BUFSZ];
static uint8_t ipx_send_buf[MC_IPX_SEND][IPX_BUFSZ];

/****************************************************************
 * Helpers
 ****************************************************************/

static void
post_listen(struct mc_ipx *x, int i)
{
    struct ipx_ecb __far *e = &ipx_recv_ecb[i];
    _fmemset(e, 0, sizeof(*e));
    e->socket[0] = (uint8_t)(x->socket >> 8);
    e->socket[1] = (uint8_t)x->socket;
    e->frag_count = 1;
    e->frag_addr = ipx_recv_buf[i];
    e->frag_size = IPX_BUFSZ;
    ipx_listen(e);              /* Listen For Packet */
}

/****************************************************************
 * Public API
 ****************************************************************/

int
mc_ipx_available(void)
{
    union REGS r;
    struct SREGS s;
    r.w.ax = 0x7A00;
    int86x(0x2F, &r, &r, &s);
    if (r.h.al != 0xFF)
        return 0;
    mc_ipx_entry = (void (__far *)(void))MK_FP(s.es, r.w.di);
    return 1;
}

int
mc_ipx_open(struct mc_ipx *x, unsigned socket)
{
    uint8_t addrbuf[10];
    int i;

    memset(x, 0, sizeof(*x));
    x->socket = (uint16_t)socket;

    if (!mc_ipx_entry && !mc_ipx_available())
        return -1;
    /*
     * Open Socket takes the socket number byte-swapped relative to its
     * on-the-wire (big-endian) form: DOSBox's OpenSocket does swapByte(DX)
     * while the ECB and packet socket fields are read big-endian, so the
     * value handed to DX must be the little-endian image of the socket.
     */
    if (ipx_open_socket((unsigned)(((socket & 0xFFu) << 8) |
                                   ((socket >> 8) & 0xFFu))) != 0)
        return -1;

    /* discover our own network and node */
    ipx_get_addr(addrbuf);
    memcpy(x->net, addrbuf, 4);
    memcpy(x->node, addrbuf + 4, 6);

    for (i = 0; i < MC_IPX_RECV; i++)
        post_listen(x, i);

    return 0;
}

void
mc_ipx_close(struct mc_ipx *x)
{
    if (x)
        ipx_close_socket(x->socket);
}

int
mc_ipx_recv(struct mc_ipx *x, void *buf, size_t buflen, struct mc_addr *from)
{
    int n, i;
    for (n = 0; n < MC_IPX_RECV; n++) {
        struct ipx_ecb __far *e;
        i = x->next_recv;
        x->next_recv = (uint16_t)((x->next_recv + 1) % MC_IPX_RECV);
        e = &ipx_recv_ecb[i];
        if (e->ccode == CC_PENDING)
            continue;                       /* still listening */
        if (e->ccode == 0) {
            struct ipx_hdr __far *h = (struct ipx_hdr __far *)ipx_recv_buf[i];
            unsigned total = ((unsigned)h->length[0] << 8) | h->length[1];
            unsigned dlen = (total > IPX_HDR_LEN) ? total - IPX_HDR_LEN : 0;
            if (dlen > buflen)
                dlen = (unsigned)buflen;
            _fmemcpy(buf, (uint8_t __far *)h + IPX_HDR_LEN, dlen);
            if (from) {
                from->len = 12;
                _fmemcpy(from->a, h->src_net, 4);
                _fmemcpy(from->a + 4, h->src_node, 6);
                from->a[10] = h->src_sock[0];
                from->a[11] = h->src_sock[1];
            }
            post_listen(x, i);              /* re-arm */
            return (int)dlen;
        }
        post_listen(x, i);                  /* error: re-arm */
    }
    return 0;
}

int
mc_ipx_send(struct mc_ipx *x, const void *buf, size_t len,
            const struct mc_addr *to)
{
    int i, slot = -1;
    struct ipx_ecb __far *e = NULL;
    struct ipx_hdr __far *h;

    if (len > MC_MTU)
        return -1;

    for (i = 0; i < MC_IPX_SEND; i++) {
        if (ipx_send_ecb[i].ccode != CC_PENDING) {
            e = &ipx_send_ecb[i];
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return 0;                           /* all send ECBs busy: drop */

    h = (struct ipx_hdr __far *)ipx_send_buf[slot];
    _fmemset(h, 0, IPX_HDR_LEN);
    h->checksum[0] = 0xFF;
    h->checksum[1] = 0xFF;
    h->length[0] = (uint8_t)((IPX_HDR_LEN + len) >> 8);
    h->length[1] = (uint8_t)(IPX_HDR_LEN + len);
    h->ptype = 4;                           /* PEP */
    _fmemcpy(h->dst_net, to->a, 4);
    _fmemcpy(h->dst_node, to->a + 4, 6);
    h->dst_sock[0] = to->a[10];
    h->dst_sock[1] = to->a[11];
    _fmemcpy((uint8_t __far *)h + IPX_HDR_LEN, buf, len);

    _fmemset(e, 0, sizeof(*e));
    e->socket[0] = (uint8_t)(x->socket >> 8);
    e->socket[1] = (uint8_t)x->socket;
    _fmemcpy(e->imm_addr, to->a + 4, 6);    /* single segment: next hop = dst */
    e->frag_count = 1;
    e->frag_addr = h;
    e->frag_size = (uint16_t)(IPX_HDR_LEN + len);
    ipx_send_pkt(e);                        /* Send Packet */
    return (int)len;
}

void
mc_ipx_local(struct mc_ipx *x, struct mc_addr *out)
{
    out->len = 12;
    memcpy(out->a, x->net, 4);
    memcpy(out->a + 4, x->node, 6);
    out->a[10] = (uint8_t)(x->socket >> 8);
    out->a[11] = (uint8_t)x->socket;
}

void
mc_ipx_broadcast(struct mc_ipx *x, struct mc_addr *out)
{
    out->len = 12;
    memcpy(out->a, x->net, 4);
    memset(out->a + 4, 0xFF, 6);
    out->a[10] = (uint8_t)(x->socket >> 8);
    out->a[11] = (uint8_t)x->socket;
}

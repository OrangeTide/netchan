/* mc_memlink.c : an in-memory datagram link for microchan tests */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "mc_memlink.h"
#include <string.h>

/*
 * Endpoint addresses are packed the way mc_udp packs them, 127.0.0.1 and a
 * port, so a test reads the same whichever transport it runs on. The core
 * only ever compares the bytes.
 */
static void
addr_for(struct mc_addr *a, int index)
{
    uint16_t port = (uint16_t)(40000 + index);

    memset(a, 0, sizeof(*a));
    a->len = 6;
    a->a[0] = 127;
    a->a[3] = 1;
    a->a[4] = (uint8_t)(port >> 8);
    a->a[5] = (uint8_t)(port & 0xff);
}

static struct meml_ep *
find_ep(struct memlink *l, const struct mc_addr *a)
{
    int i;

    for (i = 0; i < MEML_MAX_EP; i++) {
        struct meml_ep *e = &l->ep[i];
        if (e->used && e->addr.len == a->len &&
            memcmp(e->addr.a, a->a, a->len) == 0)
            return e;
    }
    return NULL;
}

static int
push(struct meml_ep *e, const struct meml_pkt *p)
{
    if (e->count >= MEML_MAX_Q)
        return -1;
    e->q[(e->head + e->count) % MEML_MAX_Q] = *p;
    e->count++;
    return 0;
}

void
meml_init(struct memlink *l)
{
    memset(l, 0, sizeof(*l));
}

int
meml_open(struct memlink *l, struct mc_addr *out)
{
    int i;

    for (i = 0; i < MEML_MAX_EP; i++) {
        if (l->ep[i].used) continue;
        memset(&l->ep[i], 0, sizeof(l->ep[i]));
        l->ep[i].used = 1;
        addr_for(&l->ep[i].addr, i);
        *out = l->ep[i].addr;
        return 0;
    }
    return -1;
}

int
meml_send(struct memlink *l, const struct mc_addr *from,
          const void *buf, size_t len, const struct mc_addr *to)
{
    struct meml_ep *dst = find_ep(l, to);
    struct meml_pkt p;
    unsigned n;

    if (!dst || len > sizeof(p.buf))
        return -1;

    memcpy(p.buf, buf, len);
    p.len = len;
    p.from = *from;

    n = ++l->offered;

    if (l->drop_every && (n % l->drop_every) == 0) {
        l->dropped++;
        return 0;
    }

    /*
     * A held datagram waits for the next one to the same endpoint and then
     * follows it in. If nothing follows, meml_recv releases it, so holding
     * delays a datagram but never loses it.
     */
    if (l->reorder_every && (n % l->reorder_every) == 0 && !dst->have_held) {
        dst->held = p;
        dst->have_held = 1;
        l->reordered++;
        return 0;
    }

    if (push(dst, &p) != 0)
        return -1;
    l->delivered++;

    if (l->dup_every && (n % l->dup_every) == 0) {
        if (push(dst, &p) == 0) {
            l->delivered++;
            l->duped++;
        }
    }

    if (dst->have_held) {
        dst->have_held = 0;
        if (push(dst, &dst->held) == 0)
            l->delivered++;
    }
    return 0;
}

int
meml_recv(struct memlink *l, const struct mc_addr *self,
          void *buf, size_t buflen, struct mc_addr *from)
{
    struct meml_ep *e = find_ep(l, self);
    struct meml_pkt *p;

    if (!e)
        return -1;

    if (e->count == 0 && e->have_held) {
        e->have_held = 0;
        if (push(e, &e->held) == 0)
            l->delivered++;
    }
    if (e->count == 0)
        return 0;

    p = &e->q[e->head];
    if (p->len > buflen)
        return -1;
    memcpy(buf, p->buf, p->len);
    if (from)
        *from = p->from;
    e->head = (e->head + 1) % MEML_MAX_Q;
    e->count--;
    return (int)p->len;
}

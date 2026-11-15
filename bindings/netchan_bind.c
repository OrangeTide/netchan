/*
 * netchan_bind.c : the flat C surface for language bindings
 *
 * Every object netchan exposes by value is wrapped here in a heap allocation
 * the shim owns, so a binding deals only in opaque pointers. The callback
 * trampolines store the binding's function pointer and context and present
 * netchan with a static C callback that forwards to it.
 *
 * Made by a machine.
 */

#include "netchan_bind.h"

#include "netchan.h"
#include "nc_udp.h"
#include "nc_crypto.h"
#include "nc_beacon.h"
#include "nc_auth.h"
#include "keystore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/socket.h>
#  include <netdb.h>
#endif

/****************************************************************
 * Small helpers
 ****************************************************************/

/* A heap copy of a C string, or NULL for a NULL input. */
static char *
dup_str(const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

/****************************************************************
 * Addresses
 ****************************************************************/

struct ncb_addr {
    struct nc_addr a;
};

ncb_addr *
ncb_addr_new(void)
{
    return calloc(1, sizeof(ncb_addr));
}

void
ncb_addr_free(ncb_addr *a)
{
    free(a);
}

int
ncb_addr_from(ncb_addr *a, const char *host, uint16_t port)
{
    struct addrinfo hints, *res, *ai;
    char service[16];
    int rc;

    if (!a || !host)
        return NCB_ERR;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);

    if (getaddrinfo(host, service, &hints, &res) != 0)
        return NCB_ERR;

    rc = NCB_ERR;
    for (ai = res; ai; ai = ai->ai_next) {
        if (nc_udp_from_sockaddr(&a->a, ai->ai_addr,
                                 (socklen_t)ai->ai_addrlen) == NC_UDP_OK) {
            rc = NCB_OK;
            break;
        }
    }
    freeaddrinfo(res);
    return rc;
}

int
ncb_addr_text(const ncb_addr *a, char *out, size_t cap, uint16_t *port_out)
{
    struct sockaddr_storage ss;
    char host[128], serv[16];
    socklen_t salen;

    if (!a || !out || cap == 0)
        return NCB_ERR;

    salen = nc_udp_to_sockaddr(&a->a, &ss);
    if (salen == 0)
        return NCB_ERR;

    if (getnameinfo((struct sockaddr *)&ss, salen, host, sizeof(host),
                    serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        return NCB_ERR;

    snprintf(out, cap, "%s", host);
    if (port_out)
        *port_out = (uint16_t)strtoul(serv, NULL, 10);
    return NCB_OK;
}

void
ncb_addr_copy(ncb_addr *dst, const ncb_addr *src)
{
    if (dst && src)
        dst->a = src->a;
}

/****************************************************************
 * Configuration
 ****************************************************************/

struct ncb_cfg {
    struct netchan_cfg c;
};

ncb_cfg *
ncb_cfg_new(void)
{
    ncb_cfg *cfg = calloc(1, sizeof(ncb_cfg));

    if (cfg)
        netchan_cfg_default(&cfg->c);
    return cfg;
}

void
ncb_cfg_free(ncb_cfg *cfg)
{
    free(cfg);
}

void
ncb_cfg_set(ncb_cfg *cfg, unsigned mtu, unsigned chan_window,
            unsigned idle_timeout_ms, unsigned retransmit_ms)
{
    if (!cfg)
        return;
    cfg->c.mtu = mtu;
    cfg->c.chan_window = chan_window;
    cfg->c.idle_timeout_ms = idle_timeout_ms;
    cfg->c.retransmit_ms = retransmit_ms;
}

void
ncb_cfg_get(const ncb_cfg *cfg, unsigned *mtu, unsigned *chan_window,
            unsigned *idle_timeout_ms, unsigned *retransmit_ms)
{
    if (!cfg)
        return;
    if (mtu)
        *mtu = cfg->c.mtu;
    if (chan_window)
        *chan_window = cfg->c.chan_window;
    if (idle_timeout_ms)
        *idle_timeout_ms = cfg->c.idle_timeout_ms;
    if (retransmit_ms)
        *retransmit_ms = cfg->c.retransmit_ms;
}

/****************************************************************
 * Version
 ****************************************************************/

const char *
ncb_version_string(void)
{
    return NETCHAN_VERSION_STRING;
}

int
ncb_version(void)
{
    return NETCHAN_VERSION;
}

/****************************************************************
 * Connection and channels
 *
 * netchan owns these, so the shim keeps no wrapper: the opaque handle is the
 * netchan pointer itself, cast through the typedef.
 ****************************************************************/

ncb_conn *
ncb_open(int server)
{
    return (ncb_conn *)netchan_open(server);
}

void
ncb_disconnect(ncb_conn *c)
{
    netchan_disconnect((struct netchan_conn *)c);
}

void
ncb_close(ncb_conn *c)
{
    netchan_close((struct netchan_conn *)c);
}

void
ncb_config(ncb_conn *c, const ncb_cfg *cfg)
{
    if (cfg)
        netchan_config((struct netchan_conn *)c, &cfg->c);
}

int
ncb_state(ncb_conn *c)
{
    return netchan_state((struct netchan_conn *)c);
}

uint32_t
ncb_id(ncb_conn *c)
{
    return netchan_id((struct netchan_conn *)c);
}

int
ncb_connect(ncb_conn *c, const ncb_addr *addr)
{
    if (!addr)
        return NETCHAN_ERR;
    return netchan_connect((struct netchan_conn *)c, &addr->a);
}

int
ncb_accept(ncb_conn *c)
{
    return netchan_accept((struct netchan_conn *)c);
}

uint32_t
ncb_peek_id(const void *pkt, size_t len)
{
    return netchan_peek_id(pkt, len);
}

int
ncb_feed(ncb_conn *c, const void *pkt, size_t len, const ncb_addr *from,
         uint32_t now_ms)
{
    return netchan_feed((struct netchan_conn *)c, pkt, len,
                        from ? &from->a : NULL, now_ms);
}

size_t
ncb_send_next(ncb_conn *c, void *buf, size_t buflen, ncb_addr *to,
              uint32_t now_ms)
{
    return netchan_send_next((struct netchan_conn *)c, buf, buflen,
                             to ? &to->a : NULL, now_ms);
}

int
ncb_service(ncb_conn *c, uint32_t now_ms)
{
    return netchan_service((struct netchan_conn *)c, now_ms);
}

ncb_chan *
ncb_chan_open(ncb_conn *c, int type, int dir, const char *content_type)
{
    return (ncb_chan *)netchan_chan_open((struct netchan_conn *)c, type, dir,
                                         content_type);
}

void
ncb_chan_close(ncb_chan *ch)
{
    netchan_chan_close((struct netchan_chan *)ch);
}

int
ncb_chan_id(ncb_chan *ch)
{
    return netchan_chan_id((struct netchan_chan *)ch);
}

int
ncb_chan_type(ncb_chan *ch)
{
    return netchan_chan_type((struct netchan_chan *)ch);
}

const char *
ncb_chan_content_type(ncb_chan *ch)
{
    return netchan_chan_content_type((struct netchan_chan *)ch);
}

int
ncb_chan_state(ncb_chan *ch)
{
    return netchan_chan_state((struct netchan_chan *)ch);
}

int
ncb_chan_write(ncb_chan *ch, const void *data, size_t len)
{
    return netchan_chan_write((struct netchan_chan *)ch, data, len);
}

int
ncb_chan_read(ncb_chan *ch, void *buf, size_t buflen)
{
    return netchan_chan_read((struct netchan_chan *)ch, buf, buflen);
}

/****************************************************************
 * Events
 ****************************************************************/

struct ncb_event {
    struct netchan_event ev;
};

ncb_event *
ncb_event_new(void)
{
    return calloc(1, sizeof(ncb_event));
}

void
ncb_event_free(ncb_event *ev)
{
    free(ev);
}

int
ncb_poll(ncb_conn *c, ncb_event *ev)
{
    if (!ev)
        return 0;
    return netchan_poll((struct netchan_conn *)c, &ev->ev);
}

int
ncb_event_type(const ncb_event *ev)
{
    return ev ? ev->ev.type : NETCHAN_EV_NONE;
}

ncb_chan *
ncb_event_chan(const ncb_event *ev)
{
    return ev ? (ncb_chan *)ev->ev.ch : NULL;
}

uint32_t
ncb_event_redirect_conn_id(const ncb_event *ev)
{
    return ev ? ev->ev.redirect_conn_id : 0;
}

void
ncb_event_redirect_addr(const ncb_event *ev, ncb_addr *out)
{
    if (ev && out)
        out->a = ev->ev.redirect_addr;
}

/****************************************************************
 * Statistics
 ****************************************************************/

void
ncb_conn_stats(ncb_conn *c, uint32_t out[4])
{
    struct netchan_conn_stats s;

    netchan_conn_stats((struct netchan_conn *)c, &s);
    out[0] = s.rtt_ms;
    out[1] = s.rtt_min_ms;
    out[2] = s.pkts_sent;
    out[3] = s.pkts_recv;
}

void
ncb_chan_stats(ncb_chan *ch, uint32_t out[4])
{
    struct netchan_chan_stats s;

    netchan_chan_stats((struct netchan_chan *)ch, &s);
    out[0] = s.msgs_sent;
    out[1] = s.msgs_acked;
    out[2] = s.retransmissions;
    out[3] = s.msgs_recv;
}

/****************************************************************
 * Crypto
 ****************************************************************/

struct ncb_crypto {
    struct nc_crypto c;
};

struct ncb_crypto_cfg {
    struct nc_crypto_cfg cfg;
    uint8_t eph_seed[32];
    uint8_t psk[32];
    uint8_t static_sk[32];
    int have_eph_seed;
    int have_psk;
    int have_static;
};

ncb_crypto_cfg *
ncb_crypto_cfg_new(void)
{
    return calloc(1, sizeof(ncb_crypto_cfg));
}

void
ncb_crypto_cfg_free(ncb_crypto_cfg *cfg)
{
    free(cfg);
}

void
ncb_crypto_cfg_set_eph_seed(ncb_crypto_cfg *cfg, const uint8_t seed[32])
{
    if (!cfg || !seed)
        return;
    memcpy(cfg->eph_seed, seed, 32);
    cfg->have_eph_seed = 1;
}

void
ncb_crypto_cfg_set_psk(ncb_crypto_cfg *cfg, const uint8_t psk[32])
{
    if (!cfg || !psk)
        return;
    memcpy(cfg->psk, psk, 32);
    cfg->have_psk = 1;
}

void
ncb_crypto_cfg_set_static_sk(ncb_crypto_cfg *cfg, const uint8_t sk[32])
{
    if (!cfg || !sk)
        return;
    memcpy(cfg->static_sk, sk, 32);
    cfg->have_static = 1;
}

void
ncb_crypto_cfg_set_require_peer_static(ncb_crypto_cfg *cfg, int require)
{
    if (cfg)
        cfg->cfg.require_peer_static = require;
}

ncb_crypto *
ncb_crypto_new(void)
{
    return calloc(1, sizeof(ncb_crypto));
}

void
ncb_crypto_free(ncb_crypto *c)
{
    if (c) {
        memset(&c->c, 0, sizeof(c->c));
        free(c);
    }
}

int
ncb_crypto_init(ncb_crypto *c, int role, const ncb_crypto_cfg *cfg)
{
    struct nc_crypto_cfg local;

    if (!c)
        return NC_CRYPTO_ERR;
    if (!cfg)
        return nc_crypto_init(&c->c, role, NULL);

    local = cfg->cfg;
    local.eph_sk_seed = cfg->have_eph_seed ? cfg->eph_seed : NULL;
    local.psk = cfg->have_psk ? cfg->psk : NULL;
    local.static_sk = cfg->have_static ? cfg->static_sk : NULL;
    local.verify_peer = NULL;
    local.verify_ctx = NULL;
    return nc_crypto_init(&c->c, role, &local);
}

void
ncb_crypto_identity_public(uint8_t out[32], const uint8_t static_sk[32])
{
    nc_crypto_identity_public(out, static_sk);
}

size_t
ncb_crypto_handshake_packet(ncb_crypto *c, uint8_t *out, size_t cap)
{
    return nc_crypto_handshake_packet(&c->c, out, cap);
}

int
ncb_crypto_ready(ncb_crypto *c)
{
    return nc_crypto_ready(&c->c) ? 1 : 0;
}

int
ncb_crypto_failed(ncb_crypto *c)
{
    return nc_crypto_failed(&c->c) ? 1 : 0;
}

int
ncb_crypto_session_id(ncb_crypto *c, uint8_t out[32])
{
    const uint8_t *sid = nc_crypto_session_id(&c->c);

    if (!sid)
        return NCB_ERR;
    memcpy(out, sid, NC_CRYPTO_SID_LEN);
    return NCB_OK;
}

long
ncb_crypto_seal(ncb_crypto *c, const uint8_t *plain, size_t len, uint8_t *out,
                size_t cap)
{
    return nc_crypto_seal(&c->c, plain, len, out, cap);
}

long
ncb_crypto_open(ncb_crypto *c, const uint8_t *pkt, size_t len, uint8_t *out,
                size_t cap)
{
    return nc_crypto_open(&c->c, pkt, len, out, cap);
}

int
ncb_crypto_peer_static(ncb_crypto *c, uint8_t out[32])
{
    if (!c->c.peer_has_static)
        return 0;
    memcpy(out, c->c.peer_static_pk, 32);
    return 1;
}

/****************************************************************
 * Discovery
 ****************************************************************/

struct ncb_beacon {
    struct nc_beacon b;
};

ncb_beacon *
ncb_beacon_new(void)
{
    return calloc(1, sizeof(ncb_beacon));
}

void
ncb_beacon_free(ncb_beacon *b)
{
    free(b);
}

void
ncb_beacon_set_game(ncb_beacon *b, uint32_t game)
{
    b->b.game = game;
}

uint32_t
ncb_beacon_game(const ncb_beacon *b)
{
    return b->b.game;
}

void
ncb_beacon_set_wire(ncb_beacon *b, uint16_t wire)
{
    b->b.wire = wire;
}

uint16_t
ncb_beacon_wire(const ncb_beacon *b)
{
    return b->b.wire;
}

void
ncb_beacon_set_version(ncb_beacon *b, uint16_t major, uint16_t minor)
{
    b->b.major = major;
    b->b.minor = minor;
}

uint16_t
ncb_beacon_major(const ncb_beacon *b)
{
    return b->b.major;
}

uint16_t
ncb_beacon_minor(const ncb_beacon *b)
{
    return b->b.minor;
}

void
ncb_beacon_set_port(ncb_beacon *b, uint16_t port)
{
    b->b.port = port;
}

uint16_t
ncb_beacon_port(const ncb_beacon *b)
{
    return b->b.port;
}

void
ncb_beacon_set_players(ncb_beacon *b, uint16_t players, uint16_t capacity)
{
    b->b.players = players;
    b->b.capacity = capacity;
}

uint16_t
ncb_beacon_players(const ncb_beacon *b)
{
    return b->b.players;
}

uint16_t
ncb_beacon_capacity(const ncb_beacon *b)
{
    return b->b.capacity;
}

void
ncb_beacon_set_modes(ncb_beacon *b, uint32_t modes)
{
    b->b.modes = modes;
}

uint32_t
ncb_beacon_modes(const ncb_beacon *b)
{
    return b->b.modes;
}

void
ncb_beacon_set_flags(ncb_beacon *b, uint32_t flags)
{
    b->b.flags = flags;
}

uint32_t
ncb_beacon_flags(const ncb_beacon *b)
{
    return b->b.flags;
}

void
ncb_beacon_set_instance(ncb_beacon *b, uint32_t instance)
{
    b->b.instance = instance;
}

uint32_t
ncb_beacon_instance(const ncb_beacon *b)
{
    return b->b.instance;
}

void
ncb_beacon_set_name(ncb_beacon *b, const char *name)
{
    snprintf(b->b.name, sizeof(b->b.name), "%s", name ? name : "");
}

const char *
ncb_beacon_name(const ncb_beacon *b)
{
    return b->b.name;
}

void
ncb_beacon_set_contact(ncb_beacon *b, const char *contact)
{
    snprintf(b->b.contact, sizeof(b->b.contact), "%s", contact ? contact : "");
}

const char *
ncb_beacon_contact(const ncb_beacon *b)
{
    return b->b.contact;
}

void
ncb_beacon_set_key(ncb_beacon *b, const uint8_t key[32])
{
    memcpy(b->b.key, key, 32);
    b->b.has_key = 1;
}

int
ncb_beacon_key(const ncb_beacon *b, uint8_t out[32])
{
    if (!b->b.has_key)
        return 0;
    memcpy(out, b->b.key, 32);
    return 1;
}

long
ncb_beacon_build(void *out, size_t outlen, const ncb_beacon *b)
{
    return nc_beacon_build(out, outlen, &b->b);
}

long
ncb_beacon_probe(void *out, size_t outlen, uint32_t game)
{
    return nc_beacon_probe(out, outlen, game);
}

int
ncb_beacon_kind(const void *pkt, size_t len)
{
    return nc_beacon_kind(pkt, len);
}

int
ncb_beacon_parse(ncb_beacon *b, const void *pkt, size_t len)
{
    return nc_beacon_parse(&b->b, pkt, len);
}

int
ncb_beacon_probe_game(const void *pkt, size_t len, uint32_t *game)
{
    return nc_beacon_probe_game(pkt, len, game);
}

/****************************************************************
 * Keystore
 ****************************************************************/

void
ncb_hex_encode(char *out, const uint8_t *in, size_t n)
{
    ks_hex_encode(out, in, n);
}

int
ncb_hex_decode(uint8_t *out, size_t n, const char *in)
{
    return ks_hex_decode(out, n, in);
}

int
ncb_known_host(const char *path, const char *host, const uint8_t pk[32],
               uint8_t stored[32])
{
    return ks_known_host(path, host, pk, stored);
}

int
ncb_known_host_add(const char *path, const char *host, const uint8_t pk[32])
{
    return ks_known_host_add(path, host, pk);
}

int
ncb_host_key(const char *path, uint8_t sk[32])
{
    return ks_host_key(path, sk);
}

int
ncb_authorized_key(const char *path, const char *user, const uint8_t pk[32])
{
    return ks_authorized_key(path, user, pk) ? 1 : 0;
}

int
ncb_check_password(const char *path, const char *user, const char *password)
{
    return ks_check_password(path, user, password) ? 1 : 0;
}

int
ncb_passwd_add(const char *path, const char *user, const char *password)
{
    return ks_passwd_add(path, user, password);
}

int
ncb_user_exists(const char *path, const char *user)
{
    return ks_user_exists(path, user) ? 1 : 0;
}

int
ncb_keyfile_generate(const char *path, const char *passphrase, uint8_t pk[32])
{
    return ks_keyfile_generate(path, passphrase, pk);
}

int
ncb_keyfile_load(const char *path, const char *passphrase, uint8_t sk[64],
                 uint8_t pk[32])
{
    return ks_keyfile_load(path, passphrase, sk, pk);
}

int
ncb_keyfile_encrypted(const char *path)
{
    return ks_keyfile_encrypted(path) ? 1 : 0;
}

/****************************************************************
 * Auth -- the form builder
 *
 * A server callback fills one of these instead of a bare struct nc_form. It
 * holds its own arrays and a string arena, and the strings it copies stay
 * alive until the next callback or until the auth object is freed, which is
 * long enough for nc_auth to serialize the form it points into.
 ****************************************************************/

#define NCB_FB_ERRORS   NC_FORM_MAX_FIELDS
#define NCB_FB_ARENA    4096

struct ncb_form_builder {
    struct nc_form form;
    struct nc_form_field fields[NC_FORM_MAX_FIELDS];
    const char *options[NC_FORM_MAX_FIELDS][NC_FORM_MAX_OPTIONS];
    struct nc_form_error errors[NCB_FB_ERRORS];
    int nfields;
    int nerrors;
    char arena[NCB_FB_ARENA];
    size_t arena_used;
};

/* Copy s into the builder's arena and return a stable pointer, or NULL when
 * the arena is full or s is NULL. */
static const char *
fb_intern(ncb_form_builder *fb, const char *s)
{
    size_t n;
    char *p;

    if (!s)
        return NULL;
    n = strlen(s) + 1;
    if (fb->arena_used + n > sizeof(fb->arena))
        return NULL;
    p = fb->arena + fb->arena_used;
    memcpy(p, s, n);
    fb->arena_used += n;
    return p;
}

static void
fb_reset(ncb_form_builder *fb)
{
    memset(fb, 0, sizeof(*fb));
}

/* Point the form at the builder's filled arrays, ready to hand to nc_auth. */
static void
fb_finish(ncb_form_builder *fb)
{
    int i;

    for (i = 0; i < fb->nfields; i++) {
        if (fb->fields[i].noptions > 0)
            fb->fields[i].options = fb->options[i];
    }
    fb->form.fields = fb->fields;
    fb->form.nfields = (uint8_t)fb->nfields;
    fb->form.errors = fb->nerrors > 0 ? fb->errors : NULL;
    fb->form.nerrors = (uint8_t)fb->nerrors;
}

void
ncb_form_builder_title(ncb_form_builder *fb, const char *title)
{
    fb->form.title = fb_intern(fb, title);
}

void
ncb_form_builder_note(ncb_form_builder *fb, const char *note)
{
    fb->form.note = fb_intern(fb, note);
}

int
ncb_form_builder_add_field(ncb_form_builder *fb, int type, int flags,
                           const char *name, const char *label,
                           const char *value, const char *pattern)
{
    struct nc_form_field *f;
    int idx;

    if (fb->nfields >= NC_FORM_MAX_FIELDS)
        return NCB_ERR;

    idx = fb->nfields++;
    f = &fb->fields[idx];
    f->type = (uint8_t)type;
    f->flags = (uint8_t)flags;
    f->name = fb_intern(fb, name);
    f->label = fb_intern(fb, label);
    f->value = fb_intern(fb, value);
    f->pattern = fb_intern(fb, pattern);
    return idx;
}

void
ncb_form_builder_field_range(ncb_form_builder *fb, int field, int32_t min,
                             int32_t max)
{
    if (field < 0 || field >= fb->nfields)
        return;
    fb->fields[field].min = min;
    fb->fields[field].max = max;
}

int
ncb_form_builder_field_option(ncb_form_builder *fb, int field,
                              const char *option)
{
    struct nc_form_field *f;

    if (field < 0 || field >= fb->nfields)
        return NCB_ERR;
    f = &fb->fields[field];
    if (f->noptions >= NC_FORM_MAX_OPTIONS)
        return NCB_ERR;
    fb->options[field][f->noptions] = fb_intern(fb, option);
    f->noptions++;
    return NCB_OK;
}

int
ncb_form_builder_add_error(ncb_form_builder *fb, const char *name,
                           const char *message)
{
    int idx;

    if (fb->nerrors >= NCB_FB_ERRORS)
        return NCB_ERR;
    idx = fb->nerrors++;
    fb->errors[idx].name = fb_intern(fb, name);
    fb->errors[idx].message = fb_intern(fb, message);
    return NCB_OK;
}

/****************************************************************
 * Auth -- the conversation
 ****************************************************************/

struct ncb_auth {
    struct nc_auth a;
    uint8_t *buf;                   /* form buffer handed to nc_auth */
    ncb_form_builder *fb;           /* reused across interactive callbacks */

    ncb_send_fn send;
    void *send_ctx;

    ncb_methods_fn methods;
    ncb_check_key_fn check_key;
    ncb_check_pass_fn check_password;
    void *cb_ctx;

    ncb_ia_begin_fn ia_begin;
    ncb_ia_submit_fn ia_submit;
    ncb_ia_resume_fn ia_resume;
    ncb_ia_cancelled_fn ia_cancelled;
    void *ia_ctx;
};

/* Trampolines. Each receives the ncb_auth as ctx and forwards to the stored
 * binding callback. */

static void
send_tramp(void *ctx, const void *msg, size_t len)
{
    ncb_auth *a = ctx;

    if (a->send)
        a->send(a->send_ctx, msg, len);
}

static unsigned
methods_tramp(void *ctx, const char *user)
{
    ncb_auth *a = ctx;

    return a->methods ? a->methods(a->cb_ctx, user) : 0;
}

static bool
check_key_tramp(void *ctx, const char *user, const uint8_t pk[32])
{
    ncb_auth *a = ctx;

    return a->check_key ? a->check_key(a->cb_ctx, user, pk) != 0 : false;
}

static bool
check_pass_tramp(void *ctx, const char *user, const char *password)
{
    ncb_auth *a = ctx;

    return a->check_password
        ? a->check_password(a->cb_ctx, user, password) != 0 : false;
}

/* Lazily get the builder, reset for a fresh form. */
static ncb_form_builder *
auth_builder(ncb_auth *a)
{
    if (!a->fb)
        a->fb = malloc(sizeof(*a->fb));
    if (a->fb)
        fb_reset(a->fb);
    return a->fb;
}

static int
ia_begin_tramp(void *ctx, unsigned method, struct nc_form *out)
{
    ncb_auth *a = ctx;
    ncb_form_builder *fb;
    int verdict;

    if (!a->ia_begin)
        return NC_AUTH_IA_DENIED;
    fb = auth_builder(a);
    if (!fb)
        return NC_AUTH_IA_DENIED;

    verdict = a->ia_begin(a->ia_ctx, method, fb);
    fb_finish(fb);
    *out = fb->form;
    return verdict;
}

static int
ia_submit_tramp(void *ctx, struct nc_auth *na, struct nc_form *out)
{
    ncb_auth *a = ctx;
    ncb_form_builder *fb;
    int verdict;

    (void)na;
    if (!a->ia_submit)
        return NC_AUTH_IA_DENIED;
    fb = auth_builder(a);
    if (!fb)
        return NC_AUTH_IA_DENIED;

    verdict = a->ia_submit(a->ia_ctx, a, fb);
    fb_finish(fb);
    *out = fb->form;
    return verdict;
}

static int
ia_resume_tramp(void *ctx, const uint8_t *tok, size_t len, struct nc_form *out)
{
    ncb_auth *a = ctx;
    ncb_form_builder *fb;
    int verdict;

    if (!a->ia_resume)
        return NC_AUTH_IA_DENIED;
    fb = auth_builder(a);
    if (!fb)
        return NC_AUTH_IA_DENIED;

    verdict = a->ia_resume(a->ia_ctx, tok, len, fb);
    fb_finish(fb);
    *out = fb->form;
    return verdict;
}

static void
ia_cancelled_tramp(void *ctx)
{
    ncb_auth *a = ctx;

    if (a->ia_cancelled)
        a->ia_cancelled(a->ia_ctx);
}

ncb_auth *
ncb_auth_new(void)
{
    return calloc(1, sizeof(ncb_auth));
}

void
ncb_auth_free(ncb_auth *a)
{
    if (!a)
        return;
    nc_auth_clear(&a->a);
    free(a->buf);
    free(a->fb);
    free(a);
}

void
ncb_auth_client_init(ncb_auth *a, const uint8_t sid[32], const char *user,
                     ncb_send_fn send, void *send_ctx)
{
    a->send = send;
    a->send_ctx = send_ctx;
    nc_auth_client_init(&a->a, sid, user, send_tramp, a);
}

void
ncb_auth_server_init(ncb_auth *a, const uint8_t sid[32], ncb_methods_fn methods,
                     ncb_check_key_fn check_key,
                     ncb_check_pass_fn check_password, void *cb_ctx,
                     ncb_send_fn send, void *send_ctx)
{
    struct nc_auth_server_cb scb;

    a->send = send;
    a->send_ctx = send_ctx;
    a->methods = methods;
    a->check_key = check_key;
    a->check_password = check_password;
    a->cb_ctx = cb_ctx;

    memset(&scb, 0, sizeof(scb));
    scb.methods = methods_tramp;
    scb.check_key = check_key ? check_key_tramp : NULL;
    scb.check_password = check_password ? check_pass_tramp : NULL;
    scb.ctx = a;
    nc_auth_server_init(&a->a, sid, &scb, send_tramp, a);
}

void
ncb_auth_server_interactive(ncb_auth *a, size_t buflen, ncb_ia_begin_fn begin,
                            ncb_ia_submit_fn submit, ncb_ia_resume_fn resume,
                            ncb_ia_cancelled_fn cancelled, void *ctx)
{
    struct nc_auth_ia_cb iacb;

    a->ia_begin = begin;
    a->ia_submit = submit;
    a->ia_resume = resume;
    a->ia_cancelled = cancelled;
    a->ia_ctx = ctx;

    if (buflen > 0) {
        a->buf = malloc(buflen);
        if (a->buf)
            nc_auth_server_buffer(&a->a, a->buf, buflen);
    }

    memset(&iacb, 0, sizeof(iacb));
    iacb.begin = begin ? ia_begin_tramp : NULL;
    iacb.submit = submit ? ia_submit_tramp : NULL;
    iacb.resume = resume ? ia_resume_tramp : NULL;
    iacb.cancelled = cancelled ? ia_cancelled_tramp : NULL;
    iacb.ctx = a;
    nc_auth_server_interactive(&a->a, &iacb);
}

void
ncb_auth_client_buffer(ncb_auth *a, size_t buflen)
{
    if (buflen == 0)
        return;
    a->buf = malloc(buflen);
    if (a->buf)
        nc_auth_client_buffer(&a->a, a->buf, buflen);
}

void
ncb_auth_start(ncb_auth *a)
{
    nc_auth_start(&a->a);
}

int
ncb_auth_feed(ncb_auth *a, const void *msg, size_t len)
{
    return nc_auth_feed(&a->a, msg, len);
}

int
ncb_auth_needs(const ncb_auth *a)
{
    return nc_auth_needs(&a->a);
}

void
ncb_auth_supply_key(ncb_auth *a, const uint8_t sk[64], const uint8_t pk[32])
{
    nc_auth_supply_key(&a->a, sk, pk);
}

void
ncb_auth_supply_password(ncb_auth *a, const char *password)
{
    nc_auth_supply_password(&a->a, password);
}

int
ncb_auth_state(const ncb_auth *a)
{
    return nc_auth_state(&a->a);
}

const char *
ncb_auth_user(const ncb_auth *a)
{
    return nc_auth_user(&a->a);
}

void
ncb_auth_signed_digest(uint8_t out[32], const uint8_t sid[32],
                       const char *user, const uint8_t pk[32])
{
    nc_auth_signed_digest(out, sid, user, pk);
}

void
ncb_auth_clear(ncb_auth *a)
{
    nc_auth_clear(&a->a);
}

void
ncb_auth_supply_method(ncb_auth *a, unsigned method)
{
    nc_auth_supply_method(&a->a, method);
}

unsigned
ncb_auth_offered(const ncb_auth *a)
{
    return nc_auth_offered(&a->a);
}

const char *
ncb_auth_waiting(const ncb_auth *a)
{
    return nc_auth_waiting(&a->a);
}

int
ncb_auth_registered(ncb_auth *a)
{
    return nc_auth_registered(&a->a) ? 1 : 0;
}

size_t
ncb_auth_pending_token(const ncb_auth *a, uint8_t *out, size_t len)
{
    return nc_auth_pending_token(&a->a, out, len);
}

void
ncb_auth_resume_pending(ncb_auth *a, const uint8_t *tok, size_t len)
{
    nc_auth_resume_pending(&a->a, tok, len);
}

void
ncb_auth_cancel(ncb_auth *a)
{
    nc_auth_cancel(&a->a);
}

const ncb_form *
ncb_auth_form(const ncb_auth *a)
{
    return (const ncb_form *)nc_auth_form(&a->a);
}

void
ncb_auth_server_token(ncb_auth *a, const uint8_t *tok, size_t len)
{
    nc_auth_server_token(&a->a, tok, len);
}

const char *
ncb_auth_value_text(const ncb_auth *a, const char *name)
{
    return nc_auth_value_text(&a->a, name);
}

int32_t
ncb_auth_value_int(const ncb_auth *a, const char *name, int32_t fallback)
{
    return nc_auth_value_int(&a->a, name, fallback);
}

int
ncb_auth_value_bool(const ncb_auth *a, const char *name, int fallback)
{
    return nc_auth_value_bool(&a->a, name, fallback != 0) ? 1 : 0;
}

uint32_t
ncb_auth_value_choice(const ncb_auth *a, const char *name)
{
    return nc_auth_value_choice(&a->a, name);
}

/****************************************************************
 * Auth -- reading a form
 ****************************************************************/

static const struct nc_form_field *
form_field(const ncb_form *f, int i)
{
    const struct nc_form *nf = (const struct nc_form *)f;

    if (!nf || i < 0 || i >= nf->nfields)
        return NULL;
    return &nf->fields[i];
}

const char *
ncb_form_title(const ncb_form *f)
{
    const struct nc_form *nf = (const struct nc_form *)f;

    return nf ? nf->title : NULL;
}

const char *
ncb_form_note(const ncb_form *f)
{
    const struct nc_form *nf = (const struct nc_form *)f;

    return nf ? nf->note : NULL;
}

int
ncb_form_nfields(const ncb_form *f)
{
    const struct nc_form *nf = (const struct nc_form *)f;

    return nf ? nf->nfields : 0;
}

int
ncb_form_field_type(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->type : 0;
}

int
ncb_form_field_flags(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->flags : 0;
}

const char *
ncb_form_field_name(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->name : NULL;
}

const char *
ncb_form_field_label(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->label : NULL;
}

const char *
ncb_form_field_value(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->value : NULL;
}

const char *
ncb_form_field_pattern(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->pattern : NULL;
}

const char *
ncb_form_field_error(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->error : NULL;
}

int
ncb_form_field_min(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->min : 0;
}

int
ncb_form_field_max(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->max : 0;
}

int
ncb_form_field_noptions(const ncb_form *f, int i)
{
    const struct nc_form_field *ff = form_field(f, i);

    return ff ? ff->noptions : 0;
}

const char *
ncb_form_field_option(const ncb_form *f, int i, int o)
{
    const struct nc_form_field *ff = form_field(f, i);

    if (!ff || !ff->options || o < 0 || o >= ff->noptions)
        return NULL;
    return ff->options[o];
}

/****************************************************************
 * Auth -- answering a form
 ****************************************************************/

struct ncb_values {
    struct nc_form_value *vals;
    char **texts;               /* owned copies, freed with the array */
    int n;
};

ncb_values *
ncb_values_new(int n)
{
    ncb_values *v;

    if (n < 0)
        n = 0;
    v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->n = n;
    v->vals = calloc((size_t)n, sizeof(*v->vals));
    v->texts = calloc((size_t)n, sizeof(*v->texts));
    if (n > 0 && (!v->vals || !v->texts)) {
        free(v->vals);
        free(v->texts);
        free(v);
        return NULL;
    }
    return v;
}

void
ncb_values_free(ncb_values *v)
{
    int i;

    if (!v)
        return;
    for (i = 0; i < v->n; i++)
        free(v->texts[i]);
    free(v->texts);
    free(v->vals);
    free(v);
}

void
ncb_values_set_text(ncb_values *v, int i, const char *text)
{
    if (!v || i < 0 || i >= v->n)
        return;
    free(v->texts[i]);
    v->texts[i] = dup_str(text);
    v->vals[i].text = v->texts[i];
}

void
ncb_values_set_number(ncb_values *v, int i, int32_t number)
{
    if (v && i >= 0 && i < v->n)
        v->vals[i].number = number;
}

void
ncb_values_set_choice(ncb_values *v, int i, uint32_t choice)
{
    if (v && i >= 0 && i < v->n)
        v->vals[i].choice = choice;
}

void
ncb_values_set_bool(ncb_values *v, int i, int on)
{
    if (v && i >= 0 && i < v->n)
        v->vals[i].on = on != 0;
}

void
ncb_auth_submit(ncb_auth *a, const ncb_values *v)
{
    if (v)
        nc_auth_submit(&a->a, v->vals, v->n);
}

/****************************************************************
 * Auth -- framing
 ****************************************************************/

struct ncb_framer {
    struct nc_auth_framer f;
    uint8_t *buf;
};

long
ncb_auth_frame(void *out, size_t outlen, const void *msg, size_t len)
{
    return nc_auth_frame(out, outlen, msg, len);
}

ncb_framer *
ncb_framer_new(size_t cap)
{
    ncb_framer *f = calloc(1, sizeof(*f));

    if (!f)
        return NULL;
    f->buf = malloc(cap);
    if (!f->buf) {
        free(f);
        return NULL;
    }
    nc_auth_framer_init(&f->f, f->buf, cap);
    return f;
}

void
ncb_framer_free(ncb_framer *f)
{
    if (!f)
        return;
    free(f->buf);
    free(f);
}

int
ncb_framer_push(ncb_framer *f, const void *bytes, size_t n)
{
    return nc_auth_framer_push(&f->f, bytes, n);
}

long
ncb_framer_next(ncb_framer *f, void *out, size_t cap)
{
    const void *msg;
    long len;

    len = nc_auth_framer_next(&f->f, &msg);
    if (len <= 0)
        return len;
    if ((size_t)len > cap)
        return NCB_ERR;
    memcpy(out, msg, (size_t)len);
    return len;
}

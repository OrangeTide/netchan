/*
 * netchan_bind.h : a flat C surface for language bindings
 *
 * The netchan libraries expose several of their objects by value: a caller
 * declares a struct nc_crypto or a struct nc_auth on its own stack and reads
 * its fields directly. That is natural from C and awkward from everywhere
 * else. A Lua or Pascal binding would have to know the exact size and layout
 * of each struct and track it across releases.
 *
 * This layer removes that coupling. Every object is an opaque pointer the
 * shim allocates and frees, every field a binding needs is reached through a
 * named accessor, and every C callback is routed through a stored function
 * pointer the binding supplies. A binding written against this header never
 * sees a netchan struct layout, so a field added upstream does not ripple
 * into two other languages.
 *
 * Return conventions follow the library being wrapped: a function that
 * returns a byte count or a netchan status code passes it straight through,
 * so the binding sees the same numbers the C API documents. Where the shim
 * itself can fail, it returns NCB_OK or NCB_ERR.
 *
 * Made by a machine.
 */

#ifndef NETCHAN_BIND_H
#define NETCHAN_BIND_H

#include <stdint.h>
#include <stddef.h>

#define NCB_OK  (0)
#define NCB_ERR (-1)

/* Opaque handles. Each is a pointer the shim owns unless a comment says the
 * lifetime belongs to netchan (conn and chan are freed through their own
 * close calls, not through a generic free). */
typedef struct ncb_conn   ncb_conn;
typedef struct ncb_chan   ncb_chan;
typedef struct ncb_addr   ncb_addr;
typedef struct ncb_cfg    ncb_cfg;
typedef struct ncb_event  ncb_event;
typedef struct ncb_crypto ncb_crypto;
typedef struct ncb_crypto_cfg ncb_crypto_cfg;
typedef struct ncb_beacon ncb_beacon;
typedef struct ncb_auth   ncb_auth;
typedef struct ncb_framer ncb_framer;
typedef struct ncb_form   ncb_form;
typedef struct ncb_form_builder ncb_form_builder;
typedef struct ncb_values ncb_values;

/****************************************************************
 * Version
 ****************************************************************/

const char *ncb_version_string(void);
int         ncb_version(void);

/****************************************************************
 * Addresses
 *
 * An nc_addr is opaque bytes to netchan. The shim packs one from a host and
 * port using the UDP transport's rules, so a script can name a peer without
 * touching sockaddr.
 ****************************************************************/

ncb_addr *ncb_addr_new(void);
void      ncb_addr_free(ncb_addr *a);

/** Fill a from a host string and port. host may be a name or a literal
 * IPv4/IPv6 address. Returns NCB_OK, or NCB_ERR if it does not resolve. */
int ncb_addr_from(ncb_addr *a, const char *host, uint16_t port);

/** Render a back to text. Writes the host into out (cap bytes) and the port
 * through port_out. Returns NCB_OK or NCB_ERR. */
int ncb_addr_text(const ncb_addr *a, char *out, size_t cap, uint16_t *port_out);

/** Copy the raw address bytes into dst. */
void ncb_addr_copy(ncb_addr *dst, const ncb_addr *src);

/****************************************************************
 * Configuration
 ****************************************************************/

ncb_cfg *ncb_cfg_new(void);          /* starts at netchan_cfg_default */
void     ncb_cfg_free(ncb_cfg *cfg);
void     ncb_cfg_set(ncb_cfg *cfg, unsigned mtu, unsigned chan_window,
                     unsigned idle_timeout_ms, unsigned retransmit_ms);
void     ncb_cfg_get(const ncb_cfg *cfg, unsigned *mtu, unsigned *chan_window,
                     unsigned *idle_timeout_ms, unsigned *retransmit_ms);

/****************************************************************
 * Connection lifecycle
 ****************************************************************/

ncb_conn *ncb_open(int server);      /* NULL on failure */
void      ncb_disconnect(ncb_conn *c);
void      ncb_close(ncb_conn *c);
void      ncb_config(ncb_conn *c, const ncb_cfg *cfg);
int       ncb_state(ncb_conn *c);
uint32_t  ncb_id(ncb_conn *c);
int       ncb_connect(ncb_conn *c, const ncb_addr *addr);
int       ncb_accept(ncb_conn *c);

/****************************************************************
 * Packet I/O
 ****************************************************************/

uint32_t ncb_peek_id(const void *pkt, size_t len);
int      ncb_feed(ncb_conn *c, const void *pkt, size_t len,
                  const ncb_addr *from, uint32_t now_ms);

/** Next outgoing packet. Returns the byte count written to buf, 0 when
 * nothing is pending. When to is non-NULL it receives the peer address. */
size_t ncb_send_next(ncb_conn *c, void *buf, size_t buflen,
                     ncb_addr *to, uint32_t now_ms);
int    ncb_service(ncb_conn *c, uint32_t now_ms);

/****************************************************************
 * Channels
 ****************************************************************/

ncb_chan   *ncb_chan_open(ncb_conn *c, int type, int dir,
                          const char *content_type);
void        ncb_chan_close(ncb_chan *ch);
int         ncb_chan_id(ncb_chan *ch);
int         ncb_chan_type(ncb_chan *ch);
const char *ncb_chan_content_type(ncb_chan *ch);
int         ncb_chan_state(ncb_chan *ch);
int         ncb_chan_write(ncb_chan *ch, const void *data, size_t len);
int         ncb_chan_read(ncb_chan *ch, void *buf, size_t buflen);

/****************************************************************
 * Events
 ****************************************************************/

ncb_event *ncb_event_new(void);
void       ncb_event_free(ncb_event *ev);

/** Drain one event into ev. Returns 1 when one was dequeued, 0 when the
 * queue is empty. */
int ncb_poll(ncb_conn *c, ncb_event *ev);

int       ncb_event_type(const ncb_event *ev);
ncb_chan *ncb_event_chan(const ncb_event *ev);
uint32_t  ncb_event_redirect_conn_id(const ncb_event *ev);
void      ncb_event_redirect_addr(const ncb_event *ev, ncb_addr *out);

/****************************************************************
 * Statistics
 *
 * Each fills a four-element array, in the field order of the matching
 * netchan struct: connection is rtt_ms, rtt_min_ms, pkts_sent, pkts_recv;
 * channel is msgs_sent, msgs_acked, retransmissions, msgs_recv.
 ****************************************************************/

void ncb_conn_stats(ncb_conn *c, uint32_t out[4]);
void ncb_chan_stats(ncb_chan *ch, uint32_t out[4]);

/****************************************************************
 * Crypto -- the encrypted transport decorator
 ****************************************************************/

ncb_crypto_cfg *ncb_crypto_cfg_new(void);
void            ncb_crypto_cfg_free(ncb_crypto_cfg *cfg);
void ncb_crypto_cfg_set_eph_seed(ncb_crypto_cfg *cfg, const uint8_t seed[32]);
void ncb_crypto_cfg_set_psk(ncb_crypto_cfg *cfg, const uint8_t psk[32]);
void ncb_crypto_cfg_set_static_sk(ncb_crypto_cfg *cfg, const uint8_t sk[32]);
void ncb_crypto_cfg_set_require_peer_static(ncb_crypto_cfg *cfg, int require);

ncb_crypto *ncb_crypto_new(void);
void        ncb_crypto_free(ncb_crypto *c);

/** role is 0 for the connecting side, 1 for the accepting side. cfg may be
 * NULL for the unauthenticated handshake. */
int    ncb_crypto_init(ncb_crypto *c, int role, const ncb_crypto_cfg *cfg);
void   ncb_crypto_identity_public(uint8_t out[32], const uint8_t static_sk[32]);
size_t ncb_crypto_handshake_packet(ncb_crypto *c, uint8_t *out, size_t cap);
int    ncb_crypto_ready(ncb_crypto *c);
int    ncb_crypto_failed(ncb_crypto *c);

/** Copy the 32-byte session id into out. Returns NCB_OK once ready, NCB_ERR
 * before that. */
int  ncb_crypto_session_id(ncb_crypto *c, uint8_t out[32]);
long ncb_crypto_seal(ncb_crypto *c, const uint8_t *plain, size_t len,
                     uint8_t *out, size_t cap);
long ncb_crypto_open(ncb_crypto *c, const uint8_t *pkt, size_t len,
                     uint8_t *out, size_t cap);

/** The peer's identity key, for a trust-on-first-use check once the session
 * is ready. Copies the key into out and returns 1 when the peer presented
 * one, 0 when it did not. */
int ncb_crypto_peer_static(ncb_crypto *c, uint8_t out[32]);

/****************************************************************
 * Discovery -- the beacon packet
 ****************************************************************/

ncb_beacon *ncb_beacon_new(void);
void        ncb_beacon_free(ncb_beacon *b);

/* Scalar fields. The names match struct nc_beacon. */
void     ncb_beacon_set_game(ncb_beacon *b, uint32_t game);
uint32_t ncb_beacon_game(const ncb_beacon *b);
void     ncb_beacon_set_wire(ncb_beacon *b, uint16_t wire);
uint16_t ncb_beacon_wire(const ncb_beacon *b);
void     ncb_beacon_set_version(ncb_beacon *b, uint16_t major, uint16_t minor);
uint16_t ncb_beacon_major(const ncb_beacon *b);
uint16_t ncb_beacon_minor(const ncb_beacon *b);
void     ncb_beacon_set_port(ncb_beacon *b, uint16_t port);
uint16_t ncb_beacon_port(const ncb_beacon *b);
void     ncb_beacon_set_players(ncb_beacon *b, uint16_t players,
                                uint16_t capacity);
uint16_t ncb_beacon_players(const ncb_beacon *b);
uint16_t ncb_beacon_capacity(const ncb_beacon *b);
void     ncb_beacon_set_modes(ncb_beacon *b, uint32_t modes);
uint32_t ncb_beacon_modes(const ncb_beacon *b);
void     ncb_beacon_set_flags(ncb_beacon *b, uint32_t flags);
uint32_t ncb_beacon_flags(const ncb_beacon *b);
void     ncb_beacon_set_instance(ncb_beacon *b, uint32_t instance);
uint32_t ncb_beacon_instance(const ncb_beacon *b);

/* Text fields. The setters truncate to the struct's capacity. The getters
 * return a pointer into the beacon, valid until it is freed or reparsed. */
void        ncb_beacon_set_name(ncb_beacon *b, const char *name);
const char *ncb_beacon_name(const ncb_beacon *b);
void        ncb_beacon_set_contact(ncb_beacon *b, const char *contact);
const char *ncb_beacon_contact(const ncb_beacon *b);

/** Set or read the server identity key. set marks the beacon as carrying a
 * key; get copies it out and returns 1 when one is present, 0 otherwise. */
void ncb_beacon_set_key(ncb_beacon *b, const uint8_t key[32]);
int  ncb_beacon_key(const ncb_beacon *b, uint8_t out[32]);

long ncb_beacon_build(void *out, size_t outlen, const ncb_beacon *b);
long ncb_beacon_probe(void *out, size_t outlen, uint32_t game);
int  ncb_beacon_kind(const void *pkt, size_t len);
int  ncb_beacon_parse(ncb_beacon *b, const void *pkt, size_t len);
int  ncb_beacon_probe_game(const void *pkt, size_t len, uint32_t *game);

/****************************************************************
 * Keystore -- the on-disk key and password files
 ****************************************************************/

void ncb_hex_encode(char *out, const uint8_t *in, size_t n);
int  ncb_hex_decode(uint8_t *out, size_t n, const char *in);

/** Look up host. Returns one of the netchan host verdicts (unknown, match,
 * changed). On a changed key, stored receives the key that was on file. */
int  ncb_known_host(const char *path, const char *host, const uint8_t pk[32],
                    uint8_t stored[32]);
int  ncb_known_host_add(const char *path, const char *host,
                        const uint8_t pk[32]);
int  ncb_host_key(const char *path, uint8_t sk[32]);
int  ncb_authorized_key(const char *path, const char *user,
                        const uint8_t pk[32]);
int  ncb_check_password(const char *path, const char *user,
                        const char *password);
int  ncb_passwd_add(const char *path, const char *user, const char *password);
int  ncb_user_exists(const char *path, const char *user);

/** Generate a client key file, optionally sealed under a passphrase (NULL or
 * "" for none). Returns NCB_OK or NCB_ERR. */
int  ncb_keyfile_generate(const char *path, const char *passphrase,
                          uint8_t pk[32]);
int  ncb_keyfile_load(const char *path, const char *passphrase,
                      uint8_t sk[64], uint8_t pk[32]);
int  ncb_keyfile_encrypted(const char *path);

/****************************************************************
 * Auth -- the login conversation
 ****************************************************************/

/* Callbacks a binding supplies. The ctx is opaque to the shim and handed
 * back unchanged. The predicate callbacks report yes with a non-zero int. */
typedef void (*ncb_send_fn)(void *ctx, const void *msg, size_t len);
typedef unsigned (*ncb_methods_fn)(void *ctx, const char *user);
typedef int (*ncb_check_key_fn)(void *ctx, const char *user,
                                const uint8_t pk[32]);
typedef int (*ncb_check_pass_fn)(void *ctx, const char *user,
                                 const char *password);

/* Interactive server callbacks. begin, submit, and resume fill the supplied
 * form builder and return one of the NC_AUTH_IA_* verdicts. */
typedef int (*ncb_ia_begin_fn)(void *ctx, unsigned method,
                               ncb_form_builder *out);
typedef int (*ncb_ia_submit_fn)(void *ctx, ncb_auth *a, ncb_form_builder *out);
typedef int (*ncb_ia_resume_fn)(void *ctx, const uint8_t *tok, size_t len,
                                ncb_form_builder *out);
typedef void (*ncb_ia_cancelled_fn)(void *ctx);

ncb_auth *ncb_auth_new(void);
void      ncb_auth_free(ncb_auth *a);

void ncb_auth_client_init(ncb_auth *a, const uint8_t sid[32], const char *user,
                          ncb_send_fn send, void *send_ctx);

/** Register the server predicate callbacks, then start the server side.
 * check_key or check_password may be NULL when that method is not offered. */
void ncb_auth_server_init(ncb_auth *a, const uint8_t sid[32],
                          ncb_methods_fn methods, ncb_check_key_fn check_key,
                          ncb_check_pass_fn check_password, void *cb_ctx,
                          ncb_send_fn send, void *send_ctx);

/** Enable the interactive method on the server with a form buffer the shim
 * owns. Pass buflen 0 to leave it unavailable. Any callback may be NULL. */
void ncb_auth_server_interactive(ncb_auth *a, size_t buflen,
                                 ncb_ia_begin_fn begin,
                                 ncb_ia_submit_fn submit,
                                 ncb_ia_resume_fn resume,
                                 ncb_ia_cancelled_fn cancelled, void *ctx);

/** Enable the interactive method on the client with a form buffer the shim
 * owns. Pass buflen 0 to leave it unavailable. */
void ncb_auth_client_buffer(ncb_auth *a, size_t buflen);

void        ncb_auth_start(ncb_auth *a);
int         ncb_auth_feed(ncb_auth *a, const void *msg, size_t len);
int         ncb_auth_needs(const ncb_auth *a);
void        ncb_auth_supply_key(ncb_auth *a, const uint8_t sk[64],
                                const uint8_t pk[32]);
void        ncb_auth_supply_password(ncb_auth *a, const char *password);
int         ncb_auth_state(const ncb_auth *a);
const char *ncb_auth_user(const ncb_auth *a);
void        ncb_auth_signed_digest(uint8_t out[32], const uint8_t sid[32],
                                   const char *user, const uint8_t pk[32]);
void        ncb_auth_clear(ncb_auth *a);

/* Client interactive flow. */
void        ncb_auth_supply_method(ncb_auth *a, unsigned method);
unsigned    ncb_auth_offered(const ncb_auth *a);
const char *ncb_auth_waiting(const ncb_auth *a);
int         ncb_auth_registered(ncb_auth *a);
size_t      ncb_auth_pending_token(const ncb_auth *a, uint8_t *out, size_t len);
void        ncb_auth_resume_pending(ncb_auth *a, const uint8_t *tok,
                                    size_t len);
void        ncb_auth_cancel(ncb_auth *a);

/** The form the client is suspended on, or NULL when it is not on a form.
 * The handle stays valid until the next ncb_auth_feed. */
const ncb_form *ncb_auth_form(const ncb_auth *a);

/* Reading a form handed over by nc_auth_form or built in a server callback. */
const char *ncb_form_title(const ncb_form *f);
const char *ncb_form_note(const ncb_form *f);
int         ncb_form_nfields(const ncb_form *f);
int         ncb_form_field_type(const ncb_form *f, int i);
int         ncb_form_field_flags(const ncb_form *f, int i);
const char *ncb_form_field_name(const ncb_form *f, int i);
const char *ncb_form_field_label(const ncb_form *f, int i);
const char *ncb_form_field_value(const ncb_form *f, int i);
const char *ncb_form_field_pattern(const ncb_form *f, int i);
const char *ncb_form_field_error(const ncb_form *f, int i);
int         ncb_form_field_min(const ncb_form *f, int i);
int         ncb_form_field_max(const ncb_form *f, int i);
int         ncb_form_field_noptions(const ncb_form *f, int i);
const char *ncb_form_field_option(const ncb_form *f, int i, int o);

/* Answering a form. Build a value array parallel to the fields, set each by
 * index, submit it, then free it. */
ncb_values *ncb_values_new(int n);
void        ncb_values_free(ncb_values *v);
void        ncb_values_set_text(ncb_values *v, int i, const char *text);
void        ncb_values_set_number(ncb_values *v, int i, int32_t number);
void        ncb_values_set_choice(ncb_values *v, int i, uint32_t choice);
void        ncb_values_set_bool(ncb_values *v, int i, int on);
void        ncb_auth_submit(ncb_auth *a, const ncb_values *v);

/* Reading a submission from inside a server submit callback. */
const char *ncb_auth_value_text(const ncb_auth *a, const char *name);
int32_t     ncb_auth_value_int(const ncb_auth *a, const char *name,
                               int32_t fallback);
int         ncb_auth_value_bool(const ncb_auth *a, const char *name,
                                int fallback);
uint32_t    ncb_auth_value_choice(const ncb_auth *a, const char *name);

void ncb_auth_server_token(ncb_auth *a, const uint8_t *tok, size_t len);

/* Building a server form inside begin, submit, or resume. The title and note
 * default to empty. add_field returns the new field's index, or NCB_ERR when
 * the form is full. */
void ncb_form_builder_title(ncb_form_builder *fb, const char *title);
void ncb_form_builder_note(ncb_form_builder *fb, const char *note);
int  ncb_form_builder_add_field(ncb_form_builder *fb, int type, int flags,
                                const char *name, const char *label,
                                const char *value, const char *pattern);
void ncb_form_builder_field_range(ncb_form_builder *fb, int field,
                                  int32_t min, int32_t max);
int  ncb_form_builder_field_option(ncb_form_builder *fb, int field,
                                   const char *option);
int  ncb_form_builder_add_error(ncb_form_builder *fb, const char *name,
                                const char *message);

/* Framing a message stream carried on a reliable channel. */
long ncb_auth_frame(void *out, size_t outlen, const void *msg, size_t len);

ncb_framer *ncb_framer_new(size_t cap);
void        ncb_framer_free(ncb_framer *f);
int         ncb_framer_push(ncb_framer *f, const void *bytes, size_t n);

/** Take the next whole message into out (cap bytes). Returns its length, 0
 * when more bytes are needed, or NCB_ERR when it does not fit in out. */
long ncb_framer_next(ncb_framer *f, void *out, size_t cap);

#endif /* NETCHAN_BIND_H */

/* nc_auth_ia.c : the interactive method, forms and all */

#include "nc_auth_int.h"
#include "microser.h"
#include "monocypher.h"

#include <string.h>

/*
 * One method carries registration, an emailed code, a one-time token, and
 * whatever an application invents later. The server sends a form, the client
 * renders it, the answers come back, and the loop repeats until the server is
 * satisfied. Nothing in this file knows what an account is.
 *
 * A form on the wire is a run of length-prefixed strings inside one ordinary
 * bytes field, each string carrying its own short textual key:
 *
 *     t=text  n=email  l=Email address  ml=64  r=1
 *     t=choice  n=shard  l=Realm  o=Ashen Coast  o=Ravenholt
 *
 * A record begins wherever t= appears, so the run is self-delimiting: no
 * nesting, no separator to get wrong, and nothing positional. Decimal numbers
 * cost a few bytes and buy a form that can be read in a hex dump.
 */

/* Record keys. One or two letters, because they are repeated per field. */
#define K_TYPE   "t"
#define K_NAME   "n"
#define K_LABEL  "l"
#define K_VALUE  "v"
#define K_PATT   "p"
#define K_SIZE   "sz"
#define K_MAXLEN "ml"
#define K_MIN    "lo"
#define K_MAX    "hi"
#define K_OPTION "o"
#define K_REQ    "r"
#define K_MULTI  "m"
#define K_ERROR  "e"

static const struct {
    const char *name;
    uint8_t     type;
} TYPE_NAMES[] = {
    { "text",     NC_FF_TEXT },
    { "password", NC_FF_PASSWORD },
    { "email",    NC_FF_EMAIL },
    { "integer",  NC_FF_INTEGER },
    { "phone",    NC_FF_PHONE },
    { "bool",     NC_FF_BOOL },
    { "choice",   NC_FF_CHOICE },
    { "note",     NC_FF_NOTE },
    { "link",     NC_FF_LINK },
};

static const char *
type_name(uint8_t t)
{
    size_t i;

    for (i = 0; i < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]); i++)
        if (TYPE_NAMES[i].type == t)
            return TYPE_NAMES[i].name;
    return NULL;
}

static uint8_t
type_value(const char *s)
{
    size_t i;

    for (i = 0; i < sizeof(TYPE_NAMES) / sizeof(TYPE_NAMES[0]); i++)
        if (strcmp(TYPE_NAMES[i].name, s) == 0)
            return TYPE_NAMES[i].type;
    return 0;
}

/** True when a NOTE or LINK, which are shown rather than answered. */
static int
type_is_prose(uint8_t t)
{
    return t == NC_FF_NOTE || t == NC_FF_LINK;
}

/****************************************************************
 * Small text conversions
 *
 * Hand-rolled rather than snprintf and strtol, to keep this library free of
 * stdio and of locale-dependent parsing. Both are a dozen lines.
 ****************************************************************/

static size_t
fmt_i32(char *out, int32_t v)
{
    char tmp[12];
    uint32_t u = v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    size_t n = 0, i = 0;

    do {
        tmp[n++] = (char)('0' + (u % 10));
        u /= 10;
    } while (u > 0);

    if (v < 0)
        out[i++] = '-';
    while (n > 0)
        out[i++] = tmp[--n];
    out[i] = '\0';
    return i;
}

/* Parse a decimal integer. Returns 0 and leaves *out alone on anything that
 * is not entirely digits, because a malformed attribute is the peer's bug and
 * silently reading half of one would hide it. */
static int
parse_i32(const char *s, int32_t *out)
{
    int32_t v = 0;
    int neg = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (*s == '\0')
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
        if (v > 214748364)
            return 0;
        v = v * 10 + (*s - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

/**
 * Match "key=" at the head of a record string. Returns the value, or NULL.
 * The comparison is exact, so "ml" never matches "m".
 */
static const char *
key_of(const char *s, const char *key)
{
    size_t n = strlen(key);

    if (strncmp(s, key, n) == 0 && s[n] == '=')
        return s + n + 1;
    return NULL;
}

/****************************************************************
 * Writing a stringlist
 ****************************************************************/

struct sink {
    uint8_t *buf;
    size_t   cap;
    size_t   used;
    int      err;
};

static void
sink_put(struct sink *sk, const char *key, const char *val, size_t vlen)
{
    size_t klen = strlen(key);
    size_t n = klen + 1 + vlen;

    if (sk->err)
        return;
    if (n > 0xffff || sk->used + 2 + n > sk->cap) {
        sk->err = 1;
        return;
    }
    sk->buf[sk->used++] = (uint8_t)(n & 0xff);
    sk->buf[sk->used++] = (uint8_t)((n >> 8) & 0xff);
    memcpy(sk->buf + sk->used, key, klen);
    sk->used += klen;
    sk->buf[sk->used++] = '=';
    if (vlen > 0)
        memcpy(sk->buf + sk->used, val, vlen);
    sk->used += vlen;
}

static void
sink_str(struct sink *sk, const char *key, const char *val)
{
    if (val)
        sink_put(sk, key, val, strlen(val));
}

static void
sink_num(struct sink *sk, const char *key, int32_t val)
{
    char tmp[12];
    size_t n = fmt_i32(tmp, val);

    sink_put(sk, key, tmp, n);
}

/****************************************************************
 * Reading a stringlist into the caller's buffer
 *
 * Each entry arrives as a 2-byte length and that many bytes, and leaves as a
 * NUL-terminated string. One byte of terminator replaces two of prefix, so
 * the unpacked form is always smaller than the packed one and the buffer that
 * held the message can hold the result.
 ****************************************************************/

struct arena {
    uint8_t *buf;
    size_t   cap;
    size_t   used;
};

static char *
arena_str(struct arena *ar, const void *s, size_t n)
{
    char *p;

    if (ar->used + n + 1 > ar->cap)
        return NULL;
    p = (char *)ar->buf + ar->used;
    if (n > 0)
        memcpy(p, s, n);
    p[n] = '\0';
    ar->used += n + 1;
    return p;
}

static void *
arena_array(struct arena *ar, size_t count, size_t size)
{
    size_t align = sizeof(void *);
    size_t at = (ar->used + align - 1) & ~(align - 1);

    if (count == 0)
        return NULL;
    if (at + count * size > ar->cap || count > ar->cap / size)
        return NULL;
    ar->used = at + count * size;
    return ar->buf + at;
}

/**
 * Unpack a run of length-prefixed strings into the arena. Returns the number
 * unpacked, or NC_AUTH_ERR if the run is malformed or does not fit.
 */
static int
unpack(struct arena *ar, const uint8_t *src, size_t len)
{
    size_t pos = 0;
    int n = 0;

    while (pos < len) {
        size_t slen;

        if (pos + 2 > len)
            return NC_AUTH_ERR;
        slen = (size_t)src[pos] | ((size_t)src[pos + 1] << 8);
        pos += 2;
        if (pos + slen > len)
            return NC_AUTH_ERR;
        if (!arena_str(ar, src + pos, slen))
            return NC_AUTH_ERR;
        pos += slen;
        n++;
    }
    return n;
}

/** Step to the string after this one. */
static const char *
next_str(const char *s)
{
    return s + strlen(s) + 1;
}

/****************************************************************
 * A form, encoded
 ****************************************************************/

/* The message the server sends: title, note, and the field run. */
static long
encode_form(uint8_t *out, size_t outlen, const struct nc_form *f)
{
    uint8_t body[NC_FORM_MAX_MSG];
    struct sink sk;
    int pos = 0;
    uint8_t i;

    if (f->nfields > NC_FORM_MAX_FIELDS)
        return NC_AUTH_ERR;

    sk.buf = body;
    sk.cap = sizeof(body);
    sk.used = 0;
    sk.err = 0;

    for (i = 0; i < f->nfields; i++) {
        const struct nc_form_field *fl = &f->fields[i];
        const char *tn = type_name(fl->type);
        const char *err = fl->error;
        uint8_t o;

        if (!tn)
            return NC_AUTH_ERR;
        sink_str(&sk, K_TYPE, tn);
        sink_str(&sk, K_NAME, fl->name);
        sink_str(&sk, K_LABEL, fl->label);
        sink_str(&sk, K_VALUE, fl->value);
        sink_str(&sk, K_PATT, fl->pattern);
        if (fl->size)
            sink_num(&sk, K_SIZE, fl->size);
        if (fl->maxlength)
            sink_num(&sk, K_MAXLEN, fl->maxlength);
        if (fl->type == NC_FF_INTEGER && fl->min != fl->max) {
            sink_num(&sk, K_MIN, fl->min);
            sink_num(&sk, K_MAX, fl->max);
        }
        if (fl->options && fl->noptions <= NC_FORM_MAX_OPTIONS)
            for (o = 0; o < fl->noptions; o++)
                sink_str(&sk, K_OPTION, fl->options[o]);
        if (fl->flags & NC_FF_REQUIRED)
            sink_str(&sk, K_REQ, "1");
        if (fl->flags & NC_FF_MULTI)
            sink_str(&sk, K_MULTI, "1");

        /* An error given in the separate array is folded into the record it
         * names, so the wire carries one thing and the caller may use either
         * shape. */
        if (!err && f->errors) {
            uint8_t e;

            for (e = 0; e < f->nerrors; e++)
                if (fl->name && f->errors[e].name &&
                    strcmp(fl->name, f->errors[e].name) == 0) {
                    err = f->errors[e].message;
                    break;
                }
        }
        sink_str(&sk, K_ERROR, err);
    }
    if (sk.err)
        return NC_AUTH_ERR;

    if (outlen < 1)
        return NC_AUTH_ERR;
    out[pos++] = MSG_IA_FORM;
    if (f->title)
        pos = ms_write_tag_bytes(out, pos, (int)outlen, F_TITLE,
                                 f->title, (uint16_t)strlen(f->title));
    if (pos >= 0 && f->note)
        pos = ms_write_tag_bytes(out, pos, (int)outlen, F_NOTE,
                                 f->note, (uint16_t)strlen(f->note));
    if (pos >= 0)
        pos = ms_write_tag_bytes(out, pos, (int)outlen, F_FIELDS,
                                 body, (uint16_t)sk.used);
    return pos < 0 ? NC_AUTH_ERR : pos;
}

/****************************************************************
 * A form, decoded
 ****************************************************************/

/* Count what a run of record strings will need before anything is carved. */
static void
count_records(const char *s, const char *end, int *nf, int *ne, int *nopt)
{
    *nf = *ne = *nopt = 0;
    for (; s < end; s = next_str(s)) {
        if (key_of(s, K_TYPE))
            (*nf)++;
        else if (key_of(s, K_ERROR))
            (*ne)++;
        else if (key_of(s, K_OPTION))
            (*nopt)++;
    }
}

static int
decode_form(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    struct arena ar;
    const uint8_t *title = NULL, *note = NULL, *blob = NULL;
    uint16_t tlen = 0, nlen = 0, blen = 0;
    const char *s, *end, *recs;
    struct nc_form_field *fields;
    struct nc_form_error *errors;
    const char **opts;
    int pos = 1, nf = 0, ne = 0, nopt = 0, fi = -1, ei = 0, oi = 0;

    if (!a->buf)
        return NC_AUTH_ERR;

    while (pos < (int)len) {
        uint8_t tag = msg[pos++];
        uint8_t field = (uint8_t)(tag >> 3), wire = (uint8_t)(tag & 7);
        const uint8_t **dst = NULL;
        uint16_t *dlen = NULL;

        if (wire != MS_WIRE_BYTES) {
            pos = ms_skip(msg, pos, (int)len, wire);
            if (pos < 0)
                return NC_AUTH_ERR;
            continue;
        }
        if (field == F_TITLE) {
            dst = &title;
            dlen = &tlen;
        } else if (field == F_NOTE) {
            dst = &note;
            dlen = &nlen;
        } else if (field == F_FIELDS) {
            dst = &blob;
            dlen = &blen;
        }
        if (!dst) {
            pos = ms_skip(msg, pos, (int)len, wire);
            if (pos < 0)
                return NC_AUTH_ERR;
            continue;
        }
        pos = ms_read_bytes(msg, pos, (int)len, dst, 0xffff, dlen);
        if (pos < 0)
            return NC_AUTH_ERR;
    }

    ar.buf = a->buf;
    ar.cap = a->buflen;
    ar.used = 0;

    memset(&a->form, 0, sizeof(a->form));
    a->form.title = title ? arena_str(&ar, title, tlen) : NULL;
    a->form.note = note ? arena_str(&ar, note, nlen) : NULL;
    if ((title && !a->form.title) || (note && !a->form.note))
        return NC_AUTH_ERR;

    recs = (const char *)ar.buf + ar.used;
    if (blob && unpack(&ar, blob, blen) < 0)
        return NC_AUTH_ERR;
    end = (const char *)ar.buf + ar.used;
    a->strings = ar.used;

    count_records(recs, end, &nf, &ne, &nopt);
    if (nf > NC_FORM_MAX_FIELDS)
        return NC_AUTH_ERR;

    fields = arena_array(&ar, (size_t)nf, sizeof(*fields));
    errors = arena_array(&ar, (size_t)ne, sizeof(*errors));
    opts = arena_array(&ar, (size_t)nopt, sizeof(*opts));
    if ((nf && !fields) || (ne && !errors) || (nopt && !opts))
        return NC_AUTH_ERR;

    for (s = recs; s < end; s = next_str(s)) {
        struct nc_form_field *fl = fi >= 0 ? &fields[fi] : NULL;
        const char *v;
        int32_t num;

        if ((v = key_of(s, K_TYPE)) != NULL) {
            fi++;
            fl = &fields[fi];
            memset(fl, 0, sizeof(*fl));
            fl->type = type_value(v);
            if (fl->type == 0)
                return NC_AUTH_ERR;
            continue;
        }
        if (!fl)
            continue;               /* an attribute before any t=; ignore */

        if ((v = key_of(s, K_NAME)) != NULL)
            fl->name = v;
        else if ((v = key_of(s, K_LABEL)) != NULL)
            fl->label = v;
        else if ((v = key_of(s, K_VALUE)) != NULL)
            fl->value = v;
        else if ((v = key_of(s, K_PATT)) != NULL)
            fl->pattern = v;
        else if ((v = key_of(s, K_SIZE)) != NULL && parse_i32(v, &num))
            fl->size = (uint16_t)num;
        else if ((v = key_of(s, K_MAXLEN)) != NULL && parse_i32(v, &num))
            fl->maxlength = (uint16_t)num;
        else if ((v = key_of(s, K_MIN)) != NULL && parse_i32(v, &num))
            fl->min = num;
        else if ((v = key_of(s, K_MAX)) != NULL && parse_i32(v, &num))
            fl->max = num;
        else if ((v = key_of(s, K_REQ)) != NULL)
            fl->flags |= NC_FF_REQUIRED;
        else if ((v = key_of(s, K_MULTI)) != NULL)
            fl->flags |= NC_FF_MULTI;
        else if ((v = key_of(s, K_OPTION)) != NULL) {
            if (fl->noptions >= NC_FORM_MAX_OPTIONS)
                return NC_AUTH_ERR;
            if (!fl->options)
                fl->options = &opts[oi];
            opts[oi++] = v;
            fl->noptions++;
        } else if ((v = key_of(s, K_ERROR)) != NULL) {
            fl->error = v;
            errors[ei].name = fl->name;
            errors[ei].message = v;
            ei++;
        }
    }

    a->form.fields = fields;
    a->form.nfields = (uint8_t)nf;
    a->form.errors = errors;
    a->form.nerrors = (uint8_t)ei;
    return NC_AUTH_OK;
}

/****************************************************************
 * Answers
 ****************************************************************/

static long
encode_answers(uint8_t *out, size_t outlen, const struct nc_form *f,
               const struct nc_form_value *vals, int n)
{
    uint8_t body[NC_FORM_MAX_MSG];
    struct sink sk;
    int pos = 0, i;

    sk.buf = body;
    sk.cap = sizeof(body);
    sk.used = 0;
    sk.err = 0;

    for (i = 0; i < n && i < (int)f->nfields; i++) {
        const struct nc_form_field *fl = &f->fields[i];

        if (type_is_prose(fl->type) || !fl->name)
            continue;
        sink_str(&sk, K_NAME, fl->name);
        switch (fl->type) {
        case NC_FF_INTEGER:
            sink_num(&sk, K_VALUE, vals[i].number);
            break;
        case NC_FF_BOOL:
            sink_str(&sk, K_VALUE, vals[i].on ? "1" : "0");
            break;
        case NC_FF_CHOICE:
            if (fl->flags & NC_FF_MULTI) {
                uint8_t o;

                for (o = 0; o < fl->noptions; o++)
                    if (vals[i].choice & (1u << o))
                        sink_num(&sk, K_VALUE, o);
            } else {
                sink_num(&sk, K_VALUE, (int32_t)vals[i].choice);
            }
            break;
        default:
            sink_str(&sk, K_VALUE, vals[i].text ? vals[i].text : "");
            break;
        }
    }
    if (sk.err) {
        crypto_wipe(body, sizeof(body));
        return NC_AUTH_ERR;
    }

    if (outlen < 1)
        return NC_AUTH_ERR;
    out[pos++] = MSG_IA_SUBMIT;
    pos = ms_write_tag_bytes(out, pos, (int)outlen, F_ANSWERS,
                             body, (uint16_t)sk.used);
    crypto_wipe(body, sizeof(body));
    return pos < 0 ? NC_AUTH_ERR : pos;
}

static int
decode_answers(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    struct arena ar;
    const uint8_t *blob = NULL;
    uint16_t blen = 0;
    int pos = 1;

    if (!a->buf)
        return NC_AUTH_ERR;

    while (pos < (int)len) {
        uint8_t tag = msg[pos++];
        uint8_t field = (uint8_t)(tag >> 3), wire = (uint8_t)(tag & 7);

        if (field == F_ANSWERS && wire == MS_WIRE_BYTES) {
            pos = ms_read_bytes(msg, pos, (int)len, &blob, 0xffff, &blen);
        } else {
            pos = ms_skip(msg, pos, (int)len, wire);
        }
        if (pos < 0)
            return NC_AUTH_ERR;
    }

    ar.buf = a->buf;
    ar.cap = a->buflen;
    ar.used = 0;
    a->strings = 0;
    if (blob && unpack(&ar, blob, blen) < 0)
        return NC_AUTH_ERR;
    a->strings = ar.used;
    return NC_AUTH_OK;
}

/**
 * Find the first value belonging to a named field. Answers arrive as an n=
 * naming the field followed by one or more v= entries, so a lookup walks to
 * the name and then takes what follows.
 */
static const char *
value_of(const struct nc_auth *a, const char *name, const char **after)
{
    const char *s = (const char *)a->buf;
    const char *end = s + a->strings;
    int mine = 0;

    if (!a->buf || !name)
        return NULL;
    for (; s < end; s = next_str(s)) {
        const char *v;

        if ((v = key_of(s, K_NAME)) != NULL) {
            mine = strcmp(v, name) == 0;
            continue;
        }
        if (mine && (v = key_of(s, K_VALUE)) != NULL) {
            if (after)
                *after = next_str(s);
            return v;
        }
    }
    return NULL;
}

const char *
nc_auth_value_text(const struct nc_auth *a, const char *name)
{
    return value_of(a, name, NULL);
}

int32_t
nc_auth_value_int(const struct nc_auth *a, const char *name, int32_t fallback)
{
    const char *v = value_of(a, name, NULL);
    int32_t out;

    if (v && parse_i32(v, &out))
        return out;
    return fallback;
}

bool
nc_auth_value_bool(const struct nc_auth *a, const char *name, bool fallback)
{
    const char *v = value_of(a, name, NULL);

    if (!v)
        return fallback;
    return v[0] == '1';
}

uint32_t
nc_auth_value_choice(const struct nc_auth *a, const char *name)
{
    const char *s, *end;
    uint32_t mask = 0;
    int mine = 0;

    if (!a->buf || !name)
        return 0;
    s = (const char *)a->buf;
    end = s + a->strings;
    for (; s < end; s = next_str(s)) {
        const char *v;
        int32_t n;

        if ((v = key_of(s, K_NAME)) != NULL) {
            mine = strcmp(v, name) == 0;
            continue;
        }
        if (mine && (v = key_of(s, K_VALUE)) != NULL &&
            parse_i32(v, &n) && n >= 0 && n < 32)
            mask |= 1u << n;
    }
    return mask;
}

/****************************************************************
 * Client
 ****************************************************************/

void
nc_auth_client_buffer(struct nc_auth *a, void *buf, size_t len)
{
    a->buf = buf;
    a->buflen = buf ? len : 0;
}

void
nc_auth_server_buffer(struct nc_auth *a, void *buf, size_t len)
{
    a->buf = buf;
    a->buflen = buf ? len : 0;
}

unsigned
nc_auth_offered(const struct nc_auth *a)
{
    return a->offered;
}

const struct nc_form *
nc_auth_form(const struct nc_auth *a)
{
    if (a->server || a->need != NC_AUTH_NEED_FORM)
        return NULL;
    return &a->form;
}

const char *
nc_auth_waiting(const struct nc_auth *a)
{
    return a->server ? NULL : a->wait_note;
}

bool
nc_auth_registered(struct nc_auth *a)
{
    bool r = a->registered != 0;

    a->registered = 0;
    return r;
}

size_t
nc_auth_pending_token(const struct nc_auth *a, uint8_t *out, size_t len)
{
    if (a->token_len == 0 || len < a->token_len)
        return 0;
    memcpy(out, a->token, a->token_len);
    return a->token_len;
}

void
nc_auth_resume_pending(struct nc_auth *a, const uint8_t *tok, size_t len)
{
    if (a->server || !tok || len == 0 || len > NC_FORM_MAX_TOKEN)
        return;
    memcpy(a->token, tok, len);
    a->token_len = (uint8_t)len;
}

void
nc_auth_supply_method(struct nc_auth *a, unsigned method)
{
    if (a->server)
        return;
    a->chosen = method;
    a->chose = 1;
    if (a->need == NC_AUTH_NEED_METHOD) {
        a->need = NC_AUTH_NEED_NOTHING;
        nc_auth_i_ia_choose(a);
    }
}

/*
 * Act on the player's answer. An interactive bit opens the method; anything
 * else means "log in normally", which stops the machine asking again and
 * hands control back to the publickey and password chain.
 */
void
nc_auth_i_ia_choose(struct nc_auth *a)
{
    uint8_t msg[3 + NC_FORM_MAX_TOKEN];
    unsigned want = a->chosen & (NC_AUTH_M_REGISTER | NC_AUTH_M_INTERACTIVE);
    int pos = 1;

    a->chose = 0;

    if (a->chosen == 0) {
        a->need = NC_AUTH_NEED_NOTHING;
        a->state = NC_AUTH_STATE_DENIED;
        return;
    }
    if (want && !(want & a->avail)) {
        /* Chosen in advance, and this server does not offer it. Ask. */
        a->need = NC_AUTH_NEED_METHOD;
        return;
    }
    if (!want) {
        a->asked = 1;
        nc_auth_i_client_try_next(a);
        return;
    }

    want &= a->avail;
    a->ia_method = (want & NC_AUTH_M_REGISTER) ? NC_AUTH_M_REGISTER
                                               : NC_AUTH_M_INTERACTIVE;
    msg[0] = MSG_IA_BEGIN;
    pos = ms_write_tag_u8(msg, pos, (int)sizeof(msg), F_METHOD,
                          (uint8_t)a->ia_method);
    if (pos >= 0 && a->token_len > 0)
        pos = ms_write_tag_bytes(msg, pos, (int)sizeof(msg), F_TOKEN,
                                 a->token, a->token_len);
    if (pos < 0) {
        a->state = NC_AUTH_STATE_DENIED;
        return;
    }
    a->need = NC_AUTH_NEED_NOTHING;
    nc_auth_i_emit(a, msg, (size_t)pos);
}

void
nc_auth_submit(struct nc_auth *a, const struct nc_form_value *vals, int n)
{
    uint8_t msg[NC_FORM_MAX_MSG];
    long len;

    if (a->server || a->need != NC_AUTH_NEED_FORM)
        return;
    a->need = NC_AUTH_NEED_NOTHING;

    if (!vals || n < 0) {
        nc_auth_cancel(a);
        return;
    }
    len = encode_answers(msg, sizeof(msg), &a->form, vals, n);
    if (len < 0) {
        a->state = NC_AUTH_STATE_DENIED;
        return;
    }
    nc_auth_i_emit(a, msg, (size_t)len);
    crypto_wipe(msg, sizeof(msg));
}

void
nc_auth_cancel(struct nc_auth *a)
{
    uint8_t msg[1];

    if (a->server || a->ia_method == 0 || a->cancelling)
        return;

    msg[0] = MSG_IA_CANCEL;
    nc_auth_i_emit(a, msg, 1);

    /* The server may have sent a form the moment a wait resolved, so one can
     * still be in flight towards us. Everything is ignored until the method
     * offer arrives and says where we ended up. */
    a->cancelling = 1;
    a->need = NC_AUTH_NEED_NOTHING;
    a->wait_note = NULL;
    a->token_len = 0;
}

int
nc_auth_i_ia_client(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    switch (msg[0]) {
    case MSG_IA_FORM:
        if (a->cancelling)
            return NC_AUTH_OK;          /* crossed our cancel; drop it */
        if (a->ia_method == 0)
            return NC_AUTH_ERR;
        if (decode_form(a, msg, len) != NC_AUTH_OK)
            return NC_AUTH_ERR;
        a->wait_note = NULL;
        a->need = NC_AUTH_NEED_FORM;
        return NC_AUTH_OK;

    case MSG_IA_WAIT: {
        const uint8_t *note = NULL, *tok = NULL;
        uint16_t nlen = 0, tlen = 0;
        int pos = 1;

        if (a->cancelling)
            return NC_AUTH_OK;
        if (a->ia_method == 0)
            return NC_AUTH_ERR;
        while (pos < (int)len) {
            uint8_t tag = msg[pos++];
            uint8_t field = (uint8_t)(tag >> 3), wire = (uint8_t)(tag & 7);

            if (wire == MS_WIRE_BYTES && field == F_NOTE)
                pos = ms_read_bytes(msg, pos, (int)len, &note, 0xffff, &nlen);
            else if (wire == MS_WIRE_BYTES && field == F_TOKEN)
                pos = ms_read_bytes(msg, pos, (int)len, &tok, 0xffff, &tlen);
            else
                pos = ms_skip(msg, pos, (int)len, wire);
            if (pos < 0)
                return NC_AUTH_ERR;
        }
        if (tok && tlen > 0 && tlen <= NC_FORM_MAX_TOKEN) {
            memcpy(a->token, tok, tlen);
            a->token_len = (uint8_t)tlen;
        }
        if (note && a->buf) {
            struct arena ar;

            ar.buf = a->buf;
            ar.cap = a->buflen;
            ar.used = 0;
            a->wait_note = arena_str(&ar, note, nlen);
            a->strings = ar.used;
        }
        a->need = NC_AUTH_NEED_NOTHING;
        return NC_AUTH_OK;
    }

    default:
        return NC_AUTH_ERR;
    }
}

/****************************************************************
 * Server
 ****************************************************************/

void
nc_auth_server_interactive(struct nc_auth *a, const struct nc_auth_ia_cb *cb)
{
    if (cb)
        a->iacb = *cb;
}

/* Tell the application a pending registration is going nowhere. Once only:
 * cancel, a denial, and nc_auth_clear can all reach this. */
void
nc_auth_i_ia_end(struct nc_auth *a)
{
    if (a->server && a->ia_method && !a->fired) {
        a->fired = 1;
        if (a->iacb.cancelled)
            a->iacb.cancelled(a->iacb.ctx);
    }
    a->ia_method = 0;
}

void
nc_auth_server_token(struct nc_auth *a, const uint8_t *tok, size_t len)
{
    if (!a->server || !tok || len == 0 || len > NC_FORM_MAX_TOKEN)
        return;
    memcpy(a->token, tok, len);
    a->token_len = (uint8_t)len;
}

static void
send_wait(struct nc_auth *a, const struct nc_form *form,
          const uint8_t *token, size_t tlen)
{
    uint8_t msg[3 + NC_FORM_MAX_LABEL + 3 + NC_FORM_MAX_TOKEN];
    const char *note = form && form->note ? form->note : "";
    size_t nlen = strlen(note);
    int pos = 1;

    /* A token given to nc_auth_server_resume wins; otherwise use whatever
     * the callback issued with nc_auth_server_token before it parked. */
    if (!token && a->token_len > 0) {
        token = a->token;
        tlen = a->token_len;
    }

    if (nlen > NC_FORM_MAX_LABEL)
        nlen = NC_FORM_MAX_LABEL;
    msg[0] = MSG_IA_WAIT;
    pos = ms_write_tag_bytes(msg, pos, (int)sizeof(msg), F_NOTE,
                             note, (uint16_t)nlen);
    if (pos >= 0 && token && tlen > 0 && tlen <= NC_FORM_MAX_TOKEN)
        pos = ms_write_tag_bytes(msg, pos, (int)sizeof(msg), F_TOKEN,
                                 token, (uint16_t)tlen);
    if (pos > 0)
        nc_auth_i_emit(a, msg, (size_t)pos);
}

/*
 * Act on what a callback decided.
 *
 * A finished registration does not authenticate anyone: it returns the
 * conversation to the method offer, and the methods callback runs again
 * because by then the answer has changed. An interactive login that finishes
 * is an ordinary success.
 */
static void
verdict(struct nc_auth *a, int what, const struct nc_form *form,
        const uint8_t *token, size_t tlen)
{
    uint8_t msg[NC_FORM_MAX_MSG];
    unsigned method = a->ia_method;
    long len;

    switch (what) {
    case NC_AUTH_IA_MORE:
        if (!form) {
            nc_auth_i_ia_end(a);
            nc_auth_i_server_deny(a, method);
            return;
        }
        len = encode_form(msg, sizeof(msg), form);
        if (len < 0) {
            a->fired = 1;
            nc_auth_i_ia_end(a);
            nc_auth_i_server_deny(a, method);
            return;
        }
        nc_auth_i_emit(a, msg, (size_t)len);
        return;

    case NC_AUTH_IA_WAIT:
        send_wait(a, form, token, tlen);
        return;

    case NC_AUTH_IA_DONE:
        a->fired = 1;               /* it finished; nothing to cancel */
        a->ia_method = 0;
        if (method == NC_AUTH_M_REGISTER) {
            a->fired = 0;
            nc_auth_i_server_offer(a);
        } else {
            a->state = NC_AUTH_STATE_OK;
            msg[0] = MSG_OK;
            nc_auth_i_emit(a, msg, 1);
        }
        return;

    default:
        a->fired = 1;               /* the server refused; it knows */
        nc_auth_i_ia_end(a);
        nc_auth_i_server_deny(a, method);
        return;
    }
}

void
nc_auth_server_resume(struct nc_auth *a, int what, const struct nc_form *form,
                      const uint8_t *token, size_t token_len)
{
    if (!a->server || a->ia_method == 0 ||
        a->state != NC_AUTH_STATE_PENDING)
        return;
    verdict(a, what, form, token, token_len);
}

int
nc_auth_i_ia_server(struct nc_auth *a, const uint8_t *msg, size_t len)
{
    struct nc_form out;

    switch (msg[0]) {
    case MSG_IA_BEGIN: {
        const uint8_t *tok = NULL;
        uint16_t tlen = 0;
        uint8_t method = 0;
        int pos = 1, rc;

        if (!a->greeted || a->ia_method)
            return NC_AUTH_ERR;
        while (pos < (int)len) {
            uint8_t tag = msg[pos++];
            uint8_t field = (uint8_t)(tag >> 3), wire = (uint8_t)(tag & 7);

            if (field == F_METHOD && wire == MS_WIRE_8)
                pos = ms_read_u8(msg, pos, (int)len, &method);
            else if (field == F_TOKEN && wire == MS_WIRE_BYTES)
                pos = ms_read_bytes(msg, pos, (int)len, &tok, 0xffff, &tlen);
            else
                pos = ms_skip(msg, pos, (int)len, wire);
            if (pos < 0)
                return NC_AUTH_ERR;
        }
        if (method != NC_AUTH_M_REGISTER && method != NC_AUTH_M_INTERACTIVE)
            return NC_AUTH_ERR;
        if (!(a->avail & method) || !a->iacb.begin) {
            nc_auth_i_server_deny(a, 0);
            return NC_AUTH_OK;
        }

        a->ia_method = method;
        a->fired = 0;
        memset(&out, 0, sizeof(out));
        if (tok && tlen > 0) {
            if (!a->iacb.resume) {
                nc_auth_i_ia_end(a);
                nc_auth_i_server_deny(a, method);
                return NC_AUTH_OK;
            }
            rc = a->iacb.resume(a->iacb.ctx, tok, tlen, &out);
        } else {
            rc = a->iacb.begin(a->iacb.ctx, method, &out);
        }
        verdict(a, rc, &out, NULL, 0);
        return NC_AUTH_OK;
    }

    case MSG_IA_SUBMIT: {
        int rc;

        if (a->ia_method == 0 || !a->iacb.submit)
            return NC_AUTH_ERR;
        if (decode_answers(a, msg, len) != NC_AUTH_OK)
            return NC_AUTH_ERR;
        memset(&out, 0, sizeof(out));
        rc = a->iacb.submit(a->iacb.ctx, a, &out);
        if (a->buf)
            crypto_wipe(a->buf, a->strings);
        a->strings = 0;
        verdict(a, rc, &out, NULL, 0);
        return NC_AUTH_OK;
    }

    case MSG_IA_CANCEL:
        if (len != 1)
            return NC_AUTH_ERR;
        if (a->ia_method == 0)
            return NC_AUTH_ERR;
        nc_auth_i_ia_end(a);
        nc_auth_i_server_offer(a);
        return NC_AUTH_OK;

    default:
        return NC_AUTH_ERR;
    }
}

/****************************************************************
 * Teardown
 ****************************************************************/

void
nc_auth_clear(struct nc_auth *a)
{
    nc_auth_i_ia_end(a);
    if (a->buf && a->buflen > 0)
        crypto_wipe(a->buf, a->buflen);
    crypto_wipe(a, sizeof(*a));
}

/****************************************************************
 * Framing
 ****************************************************************/

long
nc_auth_frame(void *out, size_t outlen, const void *msg, size_t len)
{
    uint8_t *o = out;

    if (len == 0 || len > 0xffff || outlen < len + 2)
        return NC_AUTH_ERR;
    o[0] = (uint8_t)(len & 0xff);
    o[1] = (uint8_t)((len >> 8) & 0xff);
    memcpy(o + 2, msg, len);
    return (long)(len + 2);
}

void
nc_auth_framer_init(struct nc_auth_framer *f, void *buf, size_t cap)
{
    f->buf = buf;
    f->cap = cap;
    f->len = 0;
    f->done = 0;
}

/* Drop what the previous nc_auth_framer_next handed out. Deferred to here
 * because the caller was still holding a pointer into it. */
static void
compact(struct nc_auth_framer *f)
{
    if (f->done == 0)
        return;
    memmove(f->buf, f->buf + f->done, f->len - f->done);
    f->len -= f->done;
    f->done = 0;
}

int
nc_auth_framer_push(struct nc_auth_framer *f, const void *bytes, size_t n)
{
    compact(f);
    if (n > f->cap - f->len)
        return NC_AUTH_ERR;
    memcpy(f->buf + f->len, bytes, n);
    f->len += n;
    return NC_AUTH_OK;
}

long
nc_auth_framer_next(struct nc_auth_framer *f, const void **msg)
{
    size_t need;

    compact(f);
    if (f->len < 2)
        return 0;
    need = (size_t)f->buf[0] | ((size_t)f->buf[1] << 8);
    if (need == 0)
        return NC_AUTH_ERR;
    if (f->len < need + 2)
        return 0;
    *msg = f->buf + 2;
    f->done = need + 2;
    return (long)need;
}

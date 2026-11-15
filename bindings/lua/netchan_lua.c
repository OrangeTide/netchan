/* netchan_lua.c : Lua 5.1 binding for netchan, built on the netchan_bind shim
 *
 * Made by a machine.
 *
 * Every shim handle is wrapped in a full userdata with its own metatable.
 * Methods are reached through an __index table. A connection or channel the
 * user closes with close() is not closed again by __gc. A channel keeps its
 * connection userdata alive through its environment table, and it checks that
 * connection before each use, so it never touches a freed connection.
 *
 * The auth object bridges the shim's C callbacks into Lua. The callbacks get
 * a struct auth_ctx holding a lua_State and registry references to the Lua
 * functions. Each auth method records the lua_State it was called from before
 * it enters the shim, so callbacks run on that thread. An error raised inside
 * a callback is printed to stderr and treated as a denial, because a Lua
 * longjmp must not cross the shim's C frames.
 *
 * Interactive form callbacks receive a form builder wrapper that is valid only
 * for the duration of the callback. Client forms, from auth:form(), stay valid
 * until the next auth:feed(). The auth object is kept alive while a form is.
 *
 * Lua indices are 1-based. Form fields, form value slots and form builder
 * fields all take 1-based indices and convert to the shim's 0-based ones.
 */

#include "netchan_bind.h"
#include "netchan.h"
#include "nc_crypto.h"
#include "nc_beacon.h"
#include "nc_auth.h"
#include "keystore.h"

#include <lua.h>
#include <lauxlib.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Sizes and metatable names
 ****************************************************************/

#define NCL_IO_MAX       16384   /* buffer for one channel read */
#define NCL_PKT_MAX       4096   /* buffer for one outgoing packet */
#define NCL_KEY_LEN         32   /* public key, seed, and session id */
#define NCL_SECRET_LEN      64   /* a loaded secret key */

#define MT_CONN       "netchan.conn"
#define MT_CHAN       "netchan.chan"
#define MT_ADDR       "netchan.addr"
#define MT_CFG        "netchan.cfg"
#define MT_EVENT      "netchan.event"
#define MT_CRYPTO     "netchan.crypto"
#define MT_CRYPTO_CFG "netchan.crypto_cfg"
#define MT_BEACON     "netchan.beacon"
#define MT_AUTH       "netchan.auth"
#define MT_FORM       "netchan.form"
#define MT_FB         "netchan.form_builder"
#define MT_VALUES     "netchan.values"
#define MT_FRAMER     "netchan.framer"

/****************************************************************
 * Userdata layouts
 ****************************************************************/

struct conn_ud {
    ncb_conn *c;
};

struct chan_ud {
    ncb_chan *ch;
    int owned;          /* opened by this binding, so __gc closes it */
};

struct event_ud {
    ncb_event *ev;
};

struct addr_ud {
    ncb_addr *a;
};

struct cfg_ud {
    ncb_cfg *cfg;
};

struct crypto_ud {
    ncb_crypto *c;
};

struct crypto_cfg_ud {
    ncb_crypto_cfg *cfg;
};

struct beacon_ud {
    ncb_beacon *b;
};

/* The callback state for one auth object. The shim holds a pointer to this
 * as its callback context, so it lives until the auth object is collected. */
struct auth_ctx {
    lua_State *L;
    int send_ref;
    int methods_ref;
    int check_key_ref;
    int check_pass_ref;
    int ia_begin_ref;
    int ia_submit_ref;
    int ia_resume_ref;
    int ia_cancelled_ref;
};

/* A view has owned set to 0 and ctx NULL. Views are handed to a submit
 * callback so it can read the answers; they never free the shim handle. */
struct auth_ud {
    ncb_auth *a;
    struct auth_ctx *ctx;
    int owned;
};

struct form_ud {
    const ncb_form *f;
};

struct fb_ud {
    ncb_form_builder *fb;
};

struct values_ud {
    ncb_values *v;
    int n;
};

struct framer_ud {
    ncb_framer *f;
    size_t cap;
};

struct int_const {
    const char *name;
    lua_Integer value;
};

/****************************************************************
 * Generic helpers
 ****************************************************************/

static void *
new_handle(lua_State *L, size_t size, const char *tname)
{
    void *p = lua_newuserdata(L, size);

    memset(p, 0, size);
    luaL_getmetatable(L, tname);
    lua_setmetatable(L, -2);
    return p;
}

/* Give the object at obj an environment table that holds val under key.
 * Both indices must be absolute. */
static void
set_env(lua_State *L, int obj, const char *key, int val)
{
    lua_createtable(L, 0, 1);
    lua_pushvalue(L, val);
    lua_setfield(L, -2, key);
    lua_setfenv(L, obj);
}

/* The userdata pointer stored under key in the environment of obj, or NULL. */
static void *
env_udata(lua_State *L, int obj, const char *key)
{
    void *p;

    lua_getfenv(L, obj);
    lua_getfield(L, -1, key);
    p = lua_touserdata(L, -1);
    lua_pop(L, 2);
    return p;
}

static void
set_int(lua_State *L, const char *key, lua_Integer v)
{
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

static void
set_str(lua_State *L, const char *key, const char *v)
{
    lua_pushstring(L, v);
    lua_setfield(L, -2, key);
}

/* A string field of a table, or def when the field is nil. The string is
 * still referenced by the table, so the pointer outlives the pop. */
static const char *
get_field_string(lua_State *L, int tbl, const char *key, const char *def)
{
    const char *s;

    lua_getfield(L, tbl, key);
    s = lua_isnil(L, -1) ? def : luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return s;
}

/* Read an optional number field into out. Returns 1 when the field was set. */
static int
get_field_number(lua_State *L, int tbl, const char *key, lua_Number *out)
{
    int found = 0;

    lua_getfield(L, tbl, key);
    if (!lua_isnil(L, -1)) {
        *out = luaL_checknumber(L, -1);
        found = 1;
    }
    lua_pop(L, 1);
    return found;
}

/* A string of exactly want bytes, as an unsigned byte pointer. */
static const uint8_t *
check_bytes(lua_State *L, int idx, size_t want)
{
    size_t len;
    const char *s = luaL_checklstring(L, idx, &len);

    if (len != want)
        luaL_error(L, "netchan: expected a %d byte string, got %d bytes",
                   (int)want, (int)len);
    return (const uint8_t *)s;
}

static void
define_class(lua_State *L, const char *tname, const luaL_Reg *methods,
             lua_CFunction gc)
{
    luaL_newmetatable(L, tname);
    lua_pushcfunction(L, gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_register(L, NULL, methods);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
}

/****************************************************************
 * Checks for each handle type
 ****************************************************************/

static struct conn_ud *
check_conn(lua_State *L, int idx)
{
    struct conn_ud *cu = luaL_checkudata(L, idx, MT_CONN);

    if (!cu->c)
        luaL_error(L, "netchan: connection is closed");
    return cu;
}

static struct addr_ud *
check_addr(lua_State *L, int idx)
{
    struct addr_ud *au = luaL_checkudata(L, idx, MT_ADDR);

    if (!au->a)
        luaL_error(L, "netchan: address is gone");
    return au;
}

static ncb_cfg *
check_cfg(lua_State *L, int idx)
{
    struct cfg_ud *cu = luaL_checkudata(L, idx, MT_CFG);

    if (!cu->cfg)
        luaL_error(L, "netchan: config is gone");
    return cu->cfg;
}

static struct event_ud *
check_event(lua_State *L, int idx)
{
    struct event_ud *eu = luaL_checkudata(L, idx, MT_EVENT);

    if (!eu->ev)
        luaL_error(L, "netchan: event is gone");
    return eu;
}

/* Whether the connection under a channel is still open. The channel stores
 * that connection in its environment table. */
static int
chan_alive(lua_State *L, int idx)
{
    struct conn_ud *cu = env_udata(L, idx, "conn");

    return cu && cu->c;
}

static struct chan_ud *
check_chan(lua_State *L, int idx)
{
    struct chan_ud *cu = luaL_checkudata(L, idx, MT_CHAN);

    if (!cu->ch || !chan_alive(L, idx))
        luaL_error(L, "netchan: channel is closed");
    return cu;
}

/* Wrap a channel. conn_idx is the absolute index of the connection userdata
 * that owns it, which the channel keeps alive. */
static void
wrap_chan(lua_State *L, ncb_chan *ch, int owned, int conn_idx)
{
    struct chan_ud *cu = new_handle(L, sizeof(*cu), MT_CHAN);
    int self = lua_gettop(L);

    cu->ch = ch;
    cu->owned = owned;
    set_env(L, self, "conn", conn_idx);
}

static void
wrap_addr(lua_State *L, ncb_addr *a)
{
    struct addr_ud *au = new_handle(L, sizeof(*au), MT_ADDR);

    au->a = a;
}

static struct crypto_ud *
check_crypto(lua_State *L, int idx)
{
    struct crypto_ud *cu = luaL_checkudata(L, idx, MT_CRYPTO);

    if (!cu->c)
        luaL_error(L, "netchan: crypto object is gone");
    return cu;
}

static struct beacon_ud *
check_beacon(lua_State *L, int idx)
{
    struct beacon_ud *bu = luaL_checkudata(L, idx, MT_BEACON);

    if (!bu->b)
        luaL_error(L, "netchan: beacon is gone");
    return bu;
}

static struct values_ud *
check_values(lua_State *L, int idx)
{
    struct values_ud *vu = luaL_checkudata(L, idx, MT_VALUES);

    if (!vu->v)
        luaL_error(L, "netchan: values are gone");
    return vu;
}

/* A 1-based slot number, checked against the value count. Returns 0-based. */
static int
check_slot(lua_State *L, struct values_ud *vu, int idx)
{
    int i = luaL_checkint(L, idx);

    if (i < 1 || i > vu->n)
        luaL_error(L, "netchan: value slot %d is out of range", i);
    return i - 1;
}

static struct fb_ud *
check_fb(lua_State *L, int idx)
{
    struct fb_ud *fu = luaL_checkudata(L, idx, MT_FB);

    if (!fu->fb)
        luaL_error(L, "netchan: form builder is only valid inside a callback");
    return fu;
}

static struct fb_ud *
push_fb(lua_State *L, ncb_form_builder *fb)
{
    struct fb_ud *fu = new_handle(L, sizeof(*fu), MT_FB);

    fu->fb = fb;
    return fu;
}

/* A non-owning view of an auth object, for a submit callback. */
static struct auth_ud *
push_auth_view(lua_State *L, ncb_auth *a)
{
    struct auth_ud *au = new_handle(L, sizeof(*au), MT_AUTH);

    au->a = a;
    au->ctx = NULL;
    au->owned = 0;
    return au;
}

static struct auth_ud *
check_auth(lua_State *L, int idx)
{
    struct auth_ud *au = luaL_checkudata(L, idx, MT_AUTH);

    if (!au->a || !au->ctx)
        luaL_error(L, "netchan: auth object is not usable here");
    au->ctx->L = L;
    return au;
}

/* Value reads are allowed on a view too. */
static struct auth_ud *
check_auth_any(lua_State *L, int idx)
{
    struct auth_ud *au = luaL_checkudata(L, idx, MT_AUTH);

    if (!au->a)
        luaL_error(L, "netchan: auth object is gone");
    return au;
}

/****************************************************************
 * Auth callback bridge
 ****************************************************************/

static void
unref_slot(lua_State *L, int *slot)
{
    if (*slot != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, *slot);
    *slot = LUA_NOREF;
}

/* Keep the function at idx in the registry under slot. A non-function clears
 * the slot. idx must be absolute. */
static void
store_callback(lua_State *L, int *slot, int idx)
{
    unref_slot(L, slot);
    if (lua_isfunction(L, idx)) {
        lua_pushvalue(L, idx);
        *slot = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}

static void
table_callback(lua_State *L, int tbl, const char *key, int *slot)
{
    lua_getfield(L, tbl, key);
    store_callback(L, slot, lua_gettop(L));
    lua_pop(L, 1);
}

static void
auth_ctx_init(struct auth_ctx *ac, lua_State *L)
{
    ac->L = L;
    ac->send_ref = LUA_NOREF;
    ac->methods_ref = LUA_NOREF;
    ac->check_key_ref = LUA_NOREF;
    ac->check_pass_ref = LUA_NOREF;
    ac->ia_begin_ref = LUA_NOREF;
    ac->ia_submit_ref = LUA_NOREF;
    ac->ia_resume_ref = LUA_NOREF;
    ac->ia_cancelled_ref = LUA_NOREF;
}

static void
auth_ctx_release(lua_State *L, struct auth_ctx *ac)
{
    unref_slot(L, &ac->send_ref);
    unref_slot(L, &ac->methods_ref);
    unref_slot(L, &ac->check_key_ref);
    unref_slot(L, &ac->check_pass_ref);
    unref_slot(L, &ac->ia_begin_ref);
    unref_slot(L, &ac->ia_submit_ref);
    unref_slot(L, &ac->ia_resume_ref);
    unref_slot(L, &ac->ia_cancelled_ref);
    free(ac);
}

/* Report the error on top of the stack and pop it. */
static void
callback_failed(lua_State *L, const char *where)
{
    const char *msg = lua_tostring(L, -1);

    fprintf(stderr, "netchan: the %s callback raised: %s\n", where,
            msg ? msg : "(no message)");
    lua_pop(L, 1);
}

static void
send_adapter(void *ctx, const void *msg, size_t len)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;

    if (ac->send_ref == LUA_NOREF)
        return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->send_ref);
    lua_pushlstring(L, (const char *)msg, len);
    if (lua_pcall(L, 1, 0, 0) != 0)
        callback_failed(L, "send");
}

static unsigned
methods_adapter(void *ctx, const char *user)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;
    unsigned mask = 0;

    if (ac->methods_ref == LUA_NOREF)
        return mask;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->methods_ref);
    lua_pushstring(L, user);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        callback_failed(L, "methods");
        return mask;
    }
    mask = (unsigned)lua_tointeger(L, -1);
    lua_pop(L, 1);
    return mask;
}

static int
check_key_adapter(void *ctx, const char *user, const uint8_t pk[32])
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;
    int allow = 0;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->check_key_ref);
    lua_pushstring(L, user);
    lua_pushlstring(L, (const char *)pk, NCL_KEY_LEN);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        callback_failed(L, "check_key");
        return allow;
    }
    allow = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    return allow;
}

static int
check_pass_adapter(void *ctx, const char *user, const char *password)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;
    int allow = 0;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->check_pass_ref);
    lua_pushstring(L, user);
    lua_pushstring(L, password);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        callback_failed(L, "check_password");
        return allow;
    }
    allow = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    return allow;
}

/* begin(method, fb) returns a NC_AUTH_IA_* verdict. */
static int
ia_begin_adapter(void *ctx, unsigned method, ncb_form_builder *fb)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;
    int base = lua_gettop(L);
    struct fb_ud *fu;
    int verdict = NC_AUTH_IA_DENIED;

    if (ac->ia_begin_ref == LUA_NOREF)
        return verdict;
    fu = push_fb(L, fb);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->ia_begin_ref);
    lua_pushinteger(L, (lua_Integer)method);
    lua_pushvalue(L, base + 1);
    if (lua_pcall(L, 2, 1, 0) != 0)
        callback_failed(L, "begin");
    else
        verdict = (int)lua_tointeger(L, -1);
    fu->fb = NULL;
    lua_settop(L, base);
    return verdict;
}

/* submit(auth, fb) returns a NC_AUTH_IA_* verdict. */
static int
ia_submit_adapter(void *ctx, ncb_auth *a, ncb_form_builder *fb)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;
    int base = lua_gettop(L);
    struct auth_ud *view;
    struct fb_ud *fu;
    int verdict = NC_AUTH_IA_DENIED;

    if (ac->ia_submit_ref == LUA_NOREF)
        return verdict;
    view = push_auth_view(L, a);
    fu = push_fb(L, fb);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->ia_submit_ref);
    lua_pushvalue(L, base + 1);
    lua_pushvalue(L, base + 2);
    if (lua_pcall(L, 2, 1, 0) != 0)
        callback_failed(L, "submit");
    else
        verdict = (int)lua_tointeger(L, -1);
    fu->fb = NULL;
    view->a = NULL;
    lua_settop(L, base);
    return verdict;
}

/* resume(token, fb) returns a NC_AUTH_IA_* verdict. */
static int
ia_resume_adapter(void *ctx, const uint8_t *tok, size_t len,
                  ncb_form_builder *fb)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;
    int base = lua_gettop(L);
    struct fb_ud *fu;
    int verdict = NC_AUTH_IA_DENIED;

    if (ac->ia_resume_ref == LUA_NOREF)
        return verdict;
    fu = push_fb(L, fb);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->ia_resume_ref);
    lua_pushlstring(L, (const char *)tok, len);
    lua_pushvalue(L, base + 1);
    if (lua_pcall(L, 2, 1, 0) != 0)
        callback_failed(L, "resume");
    else
        verdict = (int)lua_tointeger(L, -1);
    fu->fb = NULL;
    lua_settop(L, base);
    return verdict;
}

static void
ia_cancelled_adapter(void *ctx)
{
    struct auth_ctx *ac = ctx;
    lua_State *L = ac->L;

    if (ac->ia_cancelled_ref == LUA_NOREF)
        return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ac->ia_cancelled_ref);
    if (lua_pcall(L, 0, 0, 0) != 0)
        callback_failed(L, "cancelled");
}

/****************************************************************
 * Module-level: version, addresses, config, events
 ****************************************************************/

static int
l_version(lua_State *L)
{
    lua_pushstring(L, ncb_version_string());
    return 1;
}

static int
l_addr(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    lua_Integer port = luaL_checkinteger(L, 2);
    ncb_addr *a = ncb_addr_new();

    if (!a)
        return luaL_error(L, "netchan: out of memory");
    if (ncb_addr_from(a, host, (uint16_t)port) != NCB_OK) {
        ncb_addr_free(a);
        return luaL_error(L, "netchan: cannot resolve %s", host);
    }
    wrap_addr(L, a);
    return 1;
}

static int
l_addr_gc(lua_State *L)
{
    struct addr_ud *au = luaL_checkudata(L, 1, MT_ADDR);

    ncb_addr_free(au->a);
    au->a = NULL;
    return 0;
}

static int
l_addr_text(lua_State *L)
{
    struct addr_ud *au = check_addr(L, 1);
    char host[128];
    uint16_t port = 0;

    if (ncb_addr_text(au->a, host, sizeof(host), &port) != NCB_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, host);
    lua_pushinteger(L, port);
    return 2;
}

static int
l_cfg(lua_State *L)
{
    struct cfg_ud *cu = new_handle(L, sizeof(*cu), MT_CFG);

    cu->cfg = ncb_cfg_new();
    if (!cu->cfg)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static int
l_cfg_gc(lua_State *L)
{
    struct cfg_ud *cu = luaL_checkudata(L, 1, MT_CFG);

    ncb_cfg_free(cu->cfg);
    cu->cfg = NULL;
    return 0;
}

static int
l_cfg_set(lua_State *L)
{
    ncb_cfg *cfg = check_cfg(L, 1);
    lua_Number v;
    unsigned mtu = 0;
    unsigned win = 0;
    unsigned idle = 0;
    unsigned retx = 0;

    luaL_checktype(L, 2, LUA_TTABLE);
    ncb_cfg_get(cfg, &mtu, &win, &idle, &retx);
    if (get_field_number(L, 2, "mtu", &v))
        mtu = (unsigned)v;
    if (get_field_number(L, 2, "chan_window", &v))
        win = (unsigned)v;
    if (get_field_number(L, 2, "idle_timeout_ms", &v))
        idle = (unsigned)v;
    if (get_field_number(L, 2, "retransmit_ms", &v))
        retx = (unsigned)v;
    ncb_cfg_set(cfg, mtu, win, idle, retx);
    return 0;
}

static int
l_cfg_get(lua_State *L)
{
    ncb_cfg *cfg = check_cfg(L, 1);
    unsigned mtu = 0;
    unsigned win = 0;
    unsigned idle = 0;
    unsigned retx = 0;

    ncb_cfg_get(cfg, &mtu, &win, &idle, &retx);
    lua_createtable(L, 0, 4);
    set_int(L, "mtu", mtu);
    set_int(L, "chan_window", win);
    set_int(L, "idle_timeout_ms", idle);
    set_int(L, "retransmit_ms", retx);
    return 1;
}

static int
l_event(lua_State *L)
{
    struct event_ud *eu = new_handle(L, sizeof(*eu), MT_EVENT);

    eu->ev = ncb_event_new();
    if (!eu->ev)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static int
l_event_gc(lua_State *L)
{
    struct event_ud *eu = luaL_checkudata(L, 1, MT_EVENT);

    ncb_event_free(eu->ev);
    eu->ev = NULL;
    return 0;
}

static int
l_event_type(lua_State *L)
{
    struct event_ud *eu = check_event(L, 1);

    lua_pushinteger(L, ncb_event_type(eu->ev));
    return 1;
}

/* The channel an event refers to. The channel is not owned here, so __gc
 * does not close it. It is valid while its connection is open. */
static int
l_event_chan(lua_State *L)
{
    struct event_ud *eu = check_event(L, 1);
    ncb_chan *ch = ncb_event_chan(eu->ev);
    int conn_idx;

    if (!ch) {
        lua_pushnil(L);
        return 1;
    }
    lua_getfenv(L, 1);
    lua_getfield(L, -1, "conn");
    conn_idx = lua_gettop(L);
    wrap_chan(L, ch, 0, conn_idx);
    return 1;
}

static int
l_event_redirect_conn_id(lua_State *L)
{
    struct event_ud *eu = check_event(L, 1);

    lua_pushnumber(L, (lua_Number)ncb_event_redirect_conn_id(eu->ev));
    return 1;
}

/* redirect_addr() returns a new address; redirect_addr(addr) fills addr. */
static int
l_event_redirect_addr(lua_State *L)
{
    struct event_ud *eu = check_event(L, 1);
    struct addr_ud *au;

    if (lua_isnoneornil(L, 2)) {
        ncb_addr *a = ncb_addr_new();

        if (!a)
            return luaL_error(L, "netchan: out of memory");
        ncb_event_redirect_addr(eu->ev, a);
        wrap_addr(L, a);
        return 1;
    }
    au = check_addr(L, 2);
    ncb_event_redirect_addr(eu->ev, au->a);
    lua_pushvalue(L, 2);
    return 1;
}

/****************************************************************
 * Connections and channels
 ****************************************************************/

static int
l_open(lua_State *L)
{
    struct conn_ud *cu = new_handle(L, sizeof(*cu), MT_CONN);
    int server = luaL_checkint(L, 1);

    cu->c = ncb_open(server ? 1 : 0);
    if (!cu->c)
        return luaL_error(L, "netchan: could not open a connection");
    return 1;
}

static int
l_conn_gc(lua_State *L)
{
    struct conn_ud *cu = luaL_checkudata(L, 1, MT_CONN);

    if (cu->c) {
        ncb_close(cu->c);
        cu->c = NULL;
    }
    return 0;
}

static int
l_conn_close(lua_State *L)
{
    struct conn_ud *cu = luaL_checkudata(L, 1, MT_CONN);

    if (cu->c) {
        ncb_close(cu->c);
        cu->c = NULL;
    }
    return 0;
}

static int
l_conn_disconnect(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);

    ncb_disconnect(cu->c);
    return 0;
}

static int
l_conn_connect(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    struct addr_ud *au = check_addr(L, 2);

    lua_pushinteger(L, ncb_connect(cu->c, au->a));
    return 1;
}

static int
l_conn_accept(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);

    lua_pushinteger(L, ncb_accept(cu->c));
    return 1;
}

static int
l_conn_state(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);

    lua_pushinteger(L, ncb_state(cu->c));
    return 1;
}

static int
l_conn_id(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);

    lua_pushnumber(L, (lua_Number)ncb_id(cu->c));
    return 1;
}

static int
l_conn_config(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    struct cfg_ud *cfg = luaL_checkudata(L, 2, MT_CFG);

    ncb_config(cu->c, cfg->cfg);
    return 0;
}

static int
l_conn_feed(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    size_t len;
    const char *pkt = luaL_checklstring(L, 2, &len);
    const ncb_addr *from = NULL;
    uint32_t now;

    if (!lua_isnoneornil(L, 3))
        from = check_addr(L, 3)->a;
    now = (uint32_t)luaL_checknumber(L, 4);
    lua_pushinteger(L, ncb_feed(cu->c, pkt, len, from, now));
    return 1;
}

/* send_next(now) returns the next packet and its destination, or nil. */
static int
l_conn_send_next(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    uint32_t now = (uint32_t)luaL_checknumber(L, 2);
    uint8_t buf[NCL_PKT_MAX];
    ncb_addr *to;
    size_t n;

    to = ncb_addr_new();
    if (!to)
        return luaL_error(L, "netchan: out of memory");
    n = ncb_send_next(cu->c, buf, sizeof(buf), to, now);
    if (n == 0) {
        ncb_addr_free(to);
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)buf, n);
    wrap_addr(L, to);
    return 2;
}

static int
l_conn_service(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    uint32_t now = (uint32_t)luaL_checknumber(L, 2);

    lua_pushinteger(L, ncb_service(cu->c, now));
    return 1;
}

/* poll(event) fills the event and returns true, or returns false when the
 * queue is empty. The event keeps the connection alive while it holds a
 * channel. */
static int
l_conn_poll(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    struct event_ud *eu = check_event(L, 2);
    int got = ncb_poll(cu->c, eu->ev);

    if (got)
        set_env(L, 2, "conn", 1);
    lua_pushboolean(L, got);
    return 1;
}

static int
l_conn_chan_open(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    int type = luaL_checkint(L, 2);
    int dir = luaL_checkint(L, 3);
    const char *ctype = luaL_optstring(L, 4, "");
    ncb_chan *ch = ncb_chan_open(cu->c, type, dir, ctype);

    if (!ch) {
        lua_pushnil(L);
        return 1;
    }
    wrap_chan(L, ch, 1, 1);
    return 1;
}

static int
l_conn_stats(lua_State *L)
{
    struct conn_ud *cu = check_conn(L, 1);
    uint32_t s[4];

    ncb_conn_stats(cu->c, s);
    lua_createtable(L, 0, 4);
    set_int(L, "rtt_ms", s[0]);
    set_int(L, "rtt_min_ms", s[1]);
    set_int(L, "pkts_sent", s[2]);
    set_int(L, "pkts_recv", s[3]);
    return 1;
}

static int
l_chan_gc(lua_State *L)
{
    struct chan_ud *cu = luaL_checkudata(L, 1, MT_CHAN);

    if (cu->owned && cu->ch && chan_alive(L, 1))
        ncb_chan_close(cu->ch);
    cu->ch = NULL;
    return 0;
}

static int
l_chan_close(lua_State *L)
{
    struct chan_ud *cu = luaL_checkudata(L, 1, MT_CHAN);

    if (cu->ch && chan_alive(L, 1))
        ncb_chan_close(cu->ch);
    cu->ch = NULL;
    return 0;
}

static int
l_chan_write(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    lua_pushinteger(L, ncb_chan_write(cu->ch, data, len));
    return 1;
}

/* read() returns the next message, or nil when none is waiting. */
static int
l_chan_read(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);
    char buf[NCL_IO_MAX];
    int n = ncb_chan_read(cu->ch, buf, sizeof(buf));

    if (n < 0)
        return luaL_error(L, "netchan: channel read failed (%d)", n);
    if (n == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, buf, (size_t)n);
    return 1;
}

static int
l_chan_id(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);

    lua_pushinteger(L, ncb_chan_id(cu->ch));
    return 1;
}

static int
l_chan_type(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);

    lua_pushinteger(L, ncb_chan_type(cu->ch));
    return 1;
}

static int
l_chan_content_type(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);

    lua_pushstring(L, ncb_chan_content_type(cu->ch));
    return 1;
}

static int
l_chan_state(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);

    lua_pushinteger(L, ncb_chan_state(cu->ch));
    return 1;
}

static int
l_chan_stats(lua_State *L)
{
    struct chan_ud *cu = check_chan(L, 1);
    uint32_t s[4];

    ncb_chan_stats(cu->ch, s);
    lua_createtable(L, 0, 4);
    set_int(L, "msgs_sent", s[0]);
    set_int(L, "msgs_acked", s[1]);
    set_int(L, "retransmissions", s[2]);
    set_int(L, "msgs_recv", s[3]);
    return 1;
}

/****************************************************************
 * Crypto
 ****************************************************************/

static int
l_crypto_cfg(lua_State *L)
{
    struct crypto_cfg_ud *cu = new_handle(L, sizeof(*cu), MT_CRYPTO_CFG);

    cu->cfg = ncb_crypto_cfg_new();
    if (!cu->cfg)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static struct crypto_cfg_ud *
check_crypto_cfg(lua_State *L, int idx)
{
    struct crypto_cfg_ud *cu = luaL_checkudata(L, idx, MT_CRYPTO_CFG);

    if (!cu->cfg)
        luaL_error(L, "netchan: crypto config is gone");
    return cu;
}

static int
l_crypto_cfg_gc(lua_State *L)
{
    struct crypto_cfg_ud *cu = luaL_checkudata(L, 1, MT_CRYPTO_CFG);

    ncb_crypto_cfg_free(cu->cfg);
    cu->cfg = NULL;
    return 0;
}

static int
l_crypto_cfg_set_eph_seed(lua_State *L)
{
    struct crypto_cfg_ud *cu = check_crypto_cfg(L, 1);

    ncb_crypto_cfg_set_eph_seed(cu->cfg, check_bytes(L, 2, NCL_KEY_LEN));
    return 0;
}

static int
l_crypto_cfg_set_psk(lua_State *L)
{
    struct crypto_cfg_ud *cu = check_crypto_cfg(L, 1);

    ncb_crypto_cfg_set_psk(cu->cfg, check_bytes(L, 2, NCL_KEY_LEN));
    return 0;
}

static int
l_crypto_cfg_set_static_sk(lua_State *L)
{
    struct crypto_cfg_ud *cu = check_crypto_cfg(L, 1);

    ncb_crypto_cfg_set_static_sk(cu->cfg, check_bytes(L, 2, NCL_KEY_LEN));
    return 0;
}

static int
l_crypto_cfg_set_require_peer_static(lua_State *L)
{
    struct crypto_cfg_ud *cu = check_crypto_cfg(L, 1);

    ncb_crypto_cfg_set_require_peer_static(cu->cfg, lua_toboolean(L, 2));
    return 0;
}

static int
l_crypto(lua_State *L)
{
    struct crypto_ud *cu = new_handle(L, sizeof(*cu), MT_CRYPTO);

    cu->c = ncb_crypto_new();
    if (!cu->c)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static int
l_crypto_gc(lua_State *L)
{
    struct crypto_ud *cu = luaL_checkudata(L, 1, MT_CRYPTO);

    ncb_crypto_free(cu->c);
    cu->c = NULL;
    return 0;
}

/* init(role, cfg) with cfg optional. Returns the shim status. */
static int
l_crypto_init(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);
    int role = luaL_checkint(L, 2);
    const ncb_crypto_cfg *cfg = NULL;

    if (!lua_isnoneornil(L, 3)) {
        struct crypto_cfg_ud *cc = check_crypto_cfg(L, 3);

        cfg = cc->cfg;
    }
    lua_pushinteger(L, ncb_crypto_init(cu->c, role, cfg));
    return 1;
}

static int
l_crypto_handshake_packet(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);
    uint8_t buf[128];
    size_t n = ncb_crypto_handshake_packet(cu->c, buf, sizeof(buf));

    lua_pushlstring(L, (const char *)buf, n);
    return 1;
}

static int
l_crypto_ready(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);

    lua_pushboolean(L, ncb_crypto_ready(cu->c) != 0);
    return 1;
}

static int
l_crypto_failed(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);

    lua_pushboolean(L, ncb_crypto_failed(cu->c) != 0);
    return 1;
}

static int
l_crypto_session_id(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);
    uint8_t sid[NCL_KEY_LEN];

    if (ncb_crypto_session_id(cu->c, sid) != NCB_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)sid, sizeof(sid));
    return 1;
}

/* seal(plain) returns the sealed packet. Errors when the session is not
 * ready or the counter is exhausted. */
static int
l_crypto_seal(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);
    size_t len;
    const char *plain = luaL_checklstring(L, 2, &len);
    size_t cap = len + NC_CRYPTO_OVERHEAD;
    uint8_t *out = malloc(cap);
    long n;

    if (!out)
        return luaL_error(L, "netchan: out of memory");
    n = ncb_crypto_seal(cu->c, (const uint8_t *)plain, len, out, cap);
    if (n < 0) {
        free(out);
        return luaL_error(L, "netchan: seal failed (session not ready?)");
    }
    lua_pushlstring(L, (const char *)out, (size_t)n);
    free(out);
    return 1;
}

/* open(packet) returns the plaintext. Returns nil when the packet carried
 * no payload (a handshake packet, for example). Errors on a bad packet. */
static int
l_crypto_open(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);
    size_t len;
    const char *pkt = luaL_checklstring(L, 2, &len);
    size_t cap = len + 1;
    uint8_t *out = malloc(cap);
    long n;

    if (!out)
        return luaL_error(L, "netchan: out of memory");
    n = ncb_crypto_open(cu->c, (const uint8_t *)pkt, len, out, cap);
    if (n < 0) {
        free(out);
        return luaL_error(L, "netchan: open failed (bad or replayed packet)");
    }
    if (n == 0)
        lua_pushnil(L);
    else
        lua_pushlstring(L, (const char *)out, (size_t)n);
    free(out);
    return 1;
}

static int
l_crypto_peer_static(lua_State *L)
{
    struct crypto_ud *cu = check_crypto(L, 1);
    uint8_t pk[NCL_KEY_LEN];

    if (!ncb_crypto_peer_static(cu->c, pk)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)pk, sizeof(pk));
    return 1;
}

static int
l_crypto_identity_public(lua_State *L)
{
    const uint8_t *sk = check_bytes(L, 1, NCL_KEY_LEN);
    uint8_t pk[NCL_KEY_LEN];

    ncb_crypto_identity_public(pk, sk);
    lua_pushlstring(L, (const char *)pk, sizeof(pk));
    return 1;
}

/****************************************************************
 * Beacon
 ****************************************************************/

static int
l_beacon(lua_State *L)
{
    struct beacon_ud *bu = new_handle(L, sizeof(*bu), MT_BEACON);

    bu->b = ncb_beacon_new();
    if (!bu->b)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static int
l_beacon_gc(lua_State *L)
{
    struct beacon_ud *bu = luaL_checkudata(L, 1, MT_BEACON);

    ncb_beacon_free(bu->b);
    bu->b = NULL;
    return 0;
}

static int
l_beacon_set_game(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_game(bu->b, (uint32_t)luaL_checknumber(L, 2));
    return 0;
}

static int
l_beacon_game(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushnumber(L, (lua_Number)ncb_beacon_game(bu->b));
    return 1;
}

static int
l_beacon_set_wire(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_wire(bu->b, (uint16_t)luaL_checkinteger(L, 2));
    return 0;
}

static int
l_beacon_wire(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushinteger(L, ncb_beacon_wire(bu->b));
    return 1;
}

static int
l_beacon_set_version(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_version(bu->b, (uint16_t)luaL_checkinteger(L, 2),
                           (uint16_t)luaL_checkinteger(L, 3));
    return 0;
}

static int
l_beacon_major(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushinteger(L, ncb_beacon_major(bu->b));
    return 1;
}

static int
l_beacon_minor(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushinteger(L, ncb_beacon_minor(bu->b));
    return 1;
}

static int
l_beacon_set_port(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_port(bu->b, (uint16_t)luaL_checkinteger(L, 2));
    return 0;
}

static int
l_beacon_port(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushinteger(L, ncb_beacon_port(bu->b));
    return 1;
}

static int
l_beacon_set_players(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_players(bu->b, (uint16_t)luaL_checkinteger(L, 2),
                           (uint16_t)luaL_checkinteger(L, 3));
    return 0;
}

static int
l_beacon_players(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushinteger(L, ncb_beacon_players(bu->b));
    return 1;
}

static int
l_beacon_capacity(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushinteger(L, ncb_beacon_capacity(bu->b));
    return 1;
}

static int
l_beacon_set_modes(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_modes(bu->b, (uint32_t)luaL_checknumber(L, 2));
    return 0;
}

static int
l_beacon_modes(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushnumber(L, (lua_Number)ncb_beacon_modes(bu->b));
    return 1;
}

static int
l_beacon_set_flags(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_flags(bu->b, (uint32_t)luaL_checknumber(L, 2));
    return 0;
}

static int
l_beacon_flags(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushnumber(L, (lua_Number)ncb_beacon_flags(bu->b));
    return 1;
}

static int
l_beacon_set_instance(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_instance(bu->b, (uint32_t)luaL_checknumber(L, 2));
    return 0;
}

static int
l_beacon_instance(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushnumber(L, (lua_Number)ncb_beacon_instance(bu->b));
    return 1;
}

static int
l_beacon_set_name(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_name(bu->b, luaL_checkstring(L, 2));
    return 0;
}

static int
l_beacon_name(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushstring(L, ncb_beacon_name(bu->b));
    return 1;
}

static int
l_beacon_set_contact(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_contact(bu->b, luaL_checkstring(L, 2));
    return 0;
}

static int
l_beacon_contact(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    lua_pushstring(L, ncb_beacon_contact(bu->b));
    return 1;
}

static int
l_beacon_set_key(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);

    ncb_beacon_set_key(bu->b, check_bytes(L, 2, NCL_KEY_LEN));
    return 0;
}

static int
l_beacon_key(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);
    uint8_t key[NCL_KEY_LEN];

    if (!ncb_beacon_key(bu->b, key)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)key, sizeof(key));
    return 1;
}

static int
l_beacon_build(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);
    uint8_t buf[NC_BEACON_MAX];
    long n = ncb_beacon_build(buf, sizeof(buf), bu->b);

    if (n < 0)
        return luaL_error(L, "netchan: beacon does not fit in a packet");
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

static int
l_beacon_parse(lua_State *L)
{
    struct beacon_ud *bu = check_beacon(L, 1);
    size_t len;
    const char *pkt = luaL_checklstring(L, 2, &len);

    lua_pushboolean(L, ncb_beacon_parse(bu->b, pkt, len) == NC_BEACON_OK);
    return 1;
}

static int
l_beacon_probe(lua_State *L)
{
    uint8_t buf[NC_BEACON_MAX];
    uint32_t game = (uint32_t)luaL_checknumber(L, 1);
    long n = ncb_beacon_probe(buf, sizeof(buf), game);

    if (n < 0)
        return luaL_error(L, "netchan: probe does not fit in a packet");
    lua_pushlstring(L, (const char *)buf, (size_t)n);
    return 1;
}

static int
l_beacon_kind(lua_State *L)
{
    size_t len;
    const char *pkt = luaL_checklstring(L, 1, &len);

    lua_pushinteger(L, ncb_beacon_kind(pkt, len));
    return 1;
}

static int
l_beacon_probe_game(lua_State *L)
{
    size_t len;
    const char *pkt = luaL_checklstring(L, 1, &len);
    uint32_t game = 0;

    if (ncb_beacon_probe_game(pkt, len, &game) != NC_BEACON_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)game);
    return 1;
}

/****************************************************************
 * Keystore
 ****************************************************************/

static int
l_keyfile_generate(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *pass = luaL_optstring(L, 2, NULL);
    uint8_t pk[NCL_KEY_LEN];

    if (ncb_keyfile_generate(path, pass, pk) != NCB_OK)
        return luaL_error(L, "netchan: could not write key file %s", path);
    lua_pushlstring(L, (const char *)pk, sizeof(pk));
    return 1;
}

static int
l_keyfile_load(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *pass = luaL_optstring(L, 2, NULL);
    uint8_t sk[NCL_SECRET_LEN];
    uint8_t pk[NCL_KEY_LEN];

    if (ncb_keyfile_load(path, pass, sk, pk) != NCB_OK)
        return luaL_error(L, "netchan: could not load key file %s", path);
    lua_pushlstring(L, (const char *)sk, sizeof(sk));
    lua_pushlstring(L, (const char *)pk, sizeof(pk));
    return 2;
}

static int
l_keyfile_encrypted(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);

    lua_pushboolean(L, ncb_keyfile_encrypted(path) != 0);
    return 1;
}

/* known_host(path, host, pk) returns the verdict, and the stored key when
 * the verdict is KS_HOST_CHANGED. */
static int
l_known_host(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *host = luaL_checkstring(L, 2);
    const uint8_t *pk = check_bytes(L, 3, NCL_KEY_LEN);
    uint8_t stored[NCL_KEY_LEN];
    int verdict = ncb_known_host(path, host, pk, stored);

    lua_pushinteger(L, verdict);
    if (verdict == KS_HOST_CHANGED)
        lua_pushlstring(L, (const char *)stored, sizeof(stored));
    else
        lua_pushnil(L);
    return 2;
}

static int
l_known_host_add(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *host = luaL_checkstring(L, 2);
    const uint8_t *pk = check_bytes(L, 3, NCL_KEY_LEN);

    lua_pushboolean(L, ncb_known_host_add(path, host, pk) == NCB_OK);
    return 1;
}

static int
l_host_key(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    uint8_t sk[NCL_KEY_LEN];

    if (ncb_host_key(path, sk) != NCB_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)sk, sizeof(sk));
    return 1;
}

static int
l_authorized_key(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *user = luaL_checkstring(L, 2);
    const uint8_t *pk = check_bytes(L, 3, NCL_KEY_LEN);

    lua_pushboolean(L, ncb_authorized_key(path, user, pk) != 0);
    return 1;
}

static int
l_check_password(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *user = luaL_checkstring(L, 2);
    const char *pw = luaL_checkstring(L, 3);

    lua_pushboolean(L, ncb_check_password(path, user, pw) != 0);
    return 1;
}

static int
l_passwd_add(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *user = luaL_checkstring(L, 2);
    const char *pw = luaL_checkstring(L, 3);

    lua_pushboolean(L, ncb_passwd_add(path, user, pw) == NCB_OK);
    return 1;
}

static int
l_user_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    const char *user = luaL_checkstring(L, 2);

    lua_pushboolean(L, ncb_user_exists(path, user) != 0);
    return 1;
}

static int
l_hex_encode(lua_State *L)
{
    size_t n;
    const char *in = luaL_checklstring(L, 1, &n);
    char *out = malloc(2 * n + 1);

    if (!out)
        return luaL_error(L, "netchan: out of memory");
    ncb_hex_encode(out, (const uint8_t *)in, n);
    lua_pushlstring(L, out, 2 * n);
    free(out);
    return 1;
}

static int
l_hex_decode(lua_State *L)
{
    size_t len;
    const char *hex = luaL_checklstring(L, 1, &len);
    size_t n = len / 2;
    uint8_t *out;

    if (len % 2 != 0)
        return luaL_error(L, "netchan: hex string has an odd length");
    out = malloc(n + 1);
    if (!out)
        return luaL_error(L, "netchan: out of memory");
    if (ncb_hex_decode(out, n, hex) != NCB_OK) {
        free(out);
        return luaL_error(L, "netchan: hex string is not valid");
    }
    lua_pushlstring(L, (const char *)out, n);
    free(out);
    return 1;
}

/****************************************************************
 * Auth: lifecycle and client flow
 ****************************************************************/

static int
l_auth(lua_State *L)
{
    struct auth_ctx *ac = calloc(1, sizeof(*ac));
    ncb_auth *a = ncb_auth_new();
    struct auth_ud *au;

    if (!ac || !a) {
        free(ac);
        ncb_auth_free(a);
        return luaL_error(L, "netchan: out of memory");
    }
    auth_ctx_init(ac, L);
    au = new_handle(L, sizeof(*au), MT_AUTH);
    au->a = a;
    au->ctx = ac;
    au->owned = 1;
    return 1;
}

static int
l_auth_gc(lua_State *L)
{
    struct auth_ud *au = luaL_checkudata(L, 1, MT_AUTH);

    if (au->owned && au->a) {
        ncb_auth_free(au->a);
        au->a = NULL;
        auth_ctx_release(L, au->ctx);
        au->ctx = NULL;
    }
    return 0;
}

static int
l_auth_client_init(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    const uint8_t *sid = check_bytes(L, 2, NCL_KEY_LEN);
    const char *user = luaL_checkstring(L, 3);

    luaL_checktype(L, 4, LUA_TFUNCTION);
    store_callback(L, &au->ctx->send_ref, 4);
    ncb_auth_client_init(au->a, sid, user, send_adapter, au->ctx);
    return 0;
}

static int
l_auth_client_buffer(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    lua_Integer n = luaL_checkinteger(L, 2);

    ncb_auth_client_buffer(au->a, (size_t)n);
    return 0;
}

static int
l_auth_start(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    ncb_auth_start(au->a);
    return 0;
}

static int
l_auth_feed(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    size_t len;
    const char *msg = luaL_checklstring(L, 2, &len);

    lua_pushinteger(L, ncb_auth_feed(au->a, msg, len));
    return 1;
}

static int
l_auth_needs(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    lua_pushinteger(L, ncb_auth_needs(au->a));
    return 1;
}

static int
l_auth_supply_key(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    const uint8_t *sk = check_bytes(L, 2, NCL_SECRET_LEN);
    const uint8_t *pk = check_bytes(L, 3, NCL_KEY_LEN);

    ncb_auth_supply_key(au->a, sk, pk);
    return 0;
}

static int
l_auth_supply_password(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    ncb_auth_supply_password(au->a, luaL_checkstring(L, 2));
    return 0;
}

static int
l_auth_supply_method(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    ncb_auth_supply_method(au->a, (unsigned)luaL_checkinteger(L, 2));
    return 0;
}

static int
l_auth_offered(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    lua_pushinteger(L, ncb_auth_offered(au->a));
    return 1;
}

static int
l_auth_waiting(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    lua_pushstring(L, ncb_auth_waiting(au->a));
    return 1;
}

static int
l_auth_registered(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    lua_pushboolean(L, ncb_auth_registered(au->a) != 0);
    return 1;
}

static int
l_auth_state(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    lua_pushinteger(L, ncb_auth_state(au->a));
    return 1;
}

static int
l_auth_user(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    lua_pushstring(L, ncb_auth_user(au->a));
    return 1;
}

static int
l_auth_cancel(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);

    ncb_auth_cancel(au->a);
    return 0;
}

static int
l_auth_pending_token(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    uint8_t tok[NC_FORM_MAX_TOKEN];
    size_t n = ncb_auth_pending_token(au->a, tok, sizeof(tok));

    if (n == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, (const char *)tok, n);
    return 1;
}

static int
l_auth_resume_pending(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    size_t len;
    const char *tok = luaL_checklstring(L, 2, &len);

    ncb_auth_resume_pending(au->a, (const uint8_t *)tok, len);
    return 0;
}

static int
l_auth_submit(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    struct values_ud *vu = check_values(L, 2);

    ncb_auth_submit(au->a, vu->v);
    return 0;
}

/* form() returns the form the client is on, or nil. The form keeps the
 * auth object alive and is valid until the next feed. */
static int
l_auth_form(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    const ncb_form *f = ncb_auth_form(au->a);
    struct form_ud *fu;

    if (!f) {
        lua_pushnil(L);
        return 1;
    }
    fu = new_handle(L, sizeof(*fu), MT_FORM);
    fu->f = f;
    set_env(L, lua_gettop(L), "auth", 1);
    return 1;
}

/****************************************************************
 * Auth: server flow
 ****************************************************************/

/* server_init(sid, {methods=, check_key=, check_password=}, send) */
static int
l_auth_server_init(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    const uint8_t *sid = check_bytes(L, 2, NCL_KEY_LEN);
    struct auth_ctx *ac = au->ctx;
    ncb_check_key_fn check_key = NULL;
    ncb_check_pass_fn check_pass = NULL;

    luaL_checktype(L, 3, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    table_callback(L, 3, "methods", &ac->methods_ref);
    table_callback(L, 3, "check_key", &ac->check_key_ref);
    table_callback(L, 3, "check_password", &ac->check_pass_ref);
    store_callback(L, &ac->send_ref, 4);
    if (ac->check_key_ref != LUA_NOREF)
        check_key = check_key_adapter;
    if (ac->check_pass_ref != LUA_NOREF)
        check_pass = check_pass_adapter;
    ncb_auth_server_init(au->a, sid, methods_adapter, check_key, check_pass,
                         ac, send_adapter, ac);
    return 0;
}

/* server_interactive(buflen, {begin=, submit=, resume=, cancelled=}) */
static int
l_auth_server_interactive(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    lua_Integer buflen = luaL_checkinteger(L, 2);
    struct auth_ctx *ac = au->ctx;
    ncb_ia_begin_fn begin = NULL;
    ncb_ia_submit_fn submit = NULL;
    ncb_ia_resume_fn resume = NULL;
    ncb_ia_cancelled_fn cancelled = NULL;

    luaL_checktype(L, 3, LUA_TTABLE);
    table_callback(L, 3, "begin", &ac->ia_begin_ref);
    table_callback(L, 3, "submit", &ac->ia_submit_ref);
    table_callback(L, 3, "resume", &ac->ia_resume_ref);
    table_callback(L, 3, "cancelled", &ac->ia_cancelled_ref);
    if (ac->ia_begin_ref != LUA_NOREF)
        begin = ia_begin_adapter;
    if (ac->ia_submit_ref != LUA_NOREF)
        submit = ia_submit_adapter;
    if (ac->ia_resume_ref != LUA_NOREF)
        resume = ia_resume_adapter;
    if (ac->ia_cancelled_ref != LUA_NOREF)
        cancelled = ia_cancelled_adapter;
    ncb_auth_server_interactive(au->a, (size_t)buflen, begin, submit, resume,
                                cancelled, ac);
    return 0;
}

static int
l_auth_server_token(lua_State *L)
{
    struct auth_ud *au = check_auth(L, 1);
    size_t len;
    const char *tok = luaL_checklstring(L, 2, &len);

    ncb_auth_server_token(au->a, (const uint8_t *)tok, len);
    return 0;
}

/* Answers read inside a submit callback. They work on a view too. */
static int
l_auth_value_text(lua_State *L)
{
    struct auth_ud *au = check_auth_any(L, 1);
    const char *name = luaL_checkstring(L, 2);

    lua_pushstring(L, ncb_auth_value_text(au->a, name));
    return 1;
}

static int
l_auth_value_int(lua_State *L)
{
    struct auth_ud *au = check_auth_any(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int32_t fallback = (int32_t)luaL_optinteger(L, 3, 0);

    lua_pushinteger(L, ncb_auth_value_int(au->a, name, fallback));
    return 1;
}

static int
l_auth_value_bool(lua_State *L)
{
    struct auth_ud *au = check_auth_any(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int fallback = lua_toboolean(L, 3);

    lua_pushboolean(L, ncb_auth_value_bool(au->a, name, fallback));
    return 1;
}

static int
l_auth_value_choice(lua_State *L)
{
    struct auth_ud *au = check_auth_any(L, 1);
    const char *name = luaL_checkstring(L, 2);

    lua_pushinteger(L, ncb_auth_value_choice(au->a, name));
    return 1;
}

/* netchan.auth_signed_digest(sid, user, pk) */
static int
l_auth_signed_digest(lua_State *L)
{
    const uint8_t *sid = check_bytes(L, 1, NCL_KEY_LEN);
    const char *user = luaL_checkstring(L, 2);
    const uint8_t *pk = check_bytes(L, 3, NCL_KEY_LEN);
    uint8_t out[NCL_KEY_LEN];

    ncb_auth_signed_digest(out, sid, user, pk);
    lua_pushlstring(L, (const char *)out, sizeof(out));
    return 1;
}

/****************************************************************
 * Form reading (client side and server builder)
 ****************************************************************/

static void
push_form_field(lua_State *L, const ncb_form *f, int i)
{
    int n = ncb_form_field_noptions(f, i);
    int o;

    lua_createtable(L, 0, 12);
    set_int(L, "type", ncb_form_field_type(f, i));
    set_int(L, "flags", ncb_form_field_flags(f, i));
    set_str(L, "name", ncb_form_field_name(f, i));
    set_str(L, "label", ncb_form_field_label(f, i));
    set_str(L, "value", ncb_form_field_value(f, i));
    set_str(L, "pattern", ncb_form_field_pattern(f, i));
    set_str(L, "error", ncb_form_field_error(f, i));
    set_int(L, "min", ncb_form_field_min(f, i));
    set_int(L, "max", ncb_form_field_max(f, i));
    if (n > 0) {
        lua_createtable(L, n, 0);
        for (o = 0; o < n; o++) {
            lua_pushstring(L, ncb_form_field_option(f, i, o));
            lua_rawseti(L, -2, o + 1);
        }
        lua_setfield(L, -2, "options");
    }
}

static const ncb_form *
check_form(lua_State *L, int idx)
{
    struct form_ud *fu = luaL_checkudata(L, idx, MT_FORM);

    if (!fu->f)
        luaL_error(L, "netchan: form is gone");
    return fu->f;
}

static int
l_form_title(lua_State *L)
{
    lua_pushstring(L, ncb_form_title(check_form(L, 1)));
    return 1;
}

static int
l_form_note(lua_State *L)
{
    lua_pushstring(L, ncb_form_note(check_form(L, 1)));
    return 1;
}

static int
l_form_nfields(lua_State *L)
{
    lua_pushinteger(L, ncb_form_nfields(check_form(L, 1)));
    return 1;
}

/* field(i) returns a table describing field i, or nil when out of range. */
static int
l_form_field(lua_State *L)
{
    const ncb_form *f = check_form(L, 1);
    int i = luaL_checkint(L, 2);

    if (i < 1 || i > ncb_form_nfields(f)) {
        lua_pushnil(L);
        return 1;
    }
    push_form_field(L, f, i - 1);
    return 1;
}

/****************************************************************
 * Values: answers to a form
 ****************************************************************/

static int
l_values(lua_State *L)
{
    struct values_ud *vu = new_handle(L, sizeof(*vu), MT_VALUES);
    int n = luaL_checkint(L, 1);

    if (n < 0)
        n = 0;
    vu->v = ncb_values_new(n);
    vu->n = n;
    if (!vu->v)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static int
l_values_gc(lua_State *L)
{
    struct values_ud *vu = luaL_checkudata(L, 1, MT_VALUES);

    ncb_values_free(vu->v);
    vu->v = NULL;
    return 0;
}

static int
l_values_set_text(lua_State *L)
{
    struct values_ud *vu = check_values(L, 1);
    int i = check_slot(L, vu, 2);

    ncb_values_set_text(vu->v, i, luaL_checkstring(L, 3));
    return 0;
}

static int
l_values_set_number(lua_State *L)
{
    struct values_ud *vu = check_values(L, 1);
    int i = check_slot(L, vu, 2);

    ncb_values_set_number(vu->v, i, (int32_t)luaL_checkinteger(L, 3));
    return 0;
}

static int
l_values_set_choice(lua_State *L)
{
    struct values_ud *vu = check_values(L, 1);
    int i = check_slot(L, vu, 2);

    ncb_values_set_choice(vu->v, i, (uint32_t)luaL_checknumber(L, 3));
    return 0;
}

static int
l_values_set_bool(lua_State *L)
{
    struct values_ud *vu = check_values(L, 1);
    int i = check_slot(L, vu, 2);

    ncb_values_set_bool(vu->v, i, lua_toboolean(L, 3));
    return 0;
}

/****************************************************************
 * Form builder, inside a server callback
 ****************************************************************/

static int
l_fb_title(lua_State *L)
{
    struct fb_ud *fu = check_fb(L, 1);

    ncb_form_builder_title(fu->fb, luaL_checkstring(L, 2));
    return 0;
}

static int
l_fb_note(lua_State *L)
{
    struct fb_ud *fu = check_fb(L, 1);

    ncb_form_builder_note(fu->fb, luaL_checkstring(L, 2));
    return 0;
}

static int
l_fb_field_range(lua_State *L)
{
    struct fb_ud *fu = check_fb(L, 1);
    int i = luaL_checkint(L, 2);

    ncb_form_builder_field_range(fu->fb, i - 1, (int32_t)luaL_checkinteger(L, 3),
                                 (int32_t)luaL_checkinteger(L, 4));
    return 0;
}

static int
l_fb_field_option(lua_State *L)
{
    struct fb_ud *fu = check_fb(L, 1);
    int i = luaL_checkint(L, 2);

    if (ncb_form_builder_field_option(fu->fb, i - 1, luaL_checkstring(L, 3))
        != NCB_OK)
        return luaL_error(L, "netchan: cannot add option to field %d", i);
    return 0;
}

static int
l_fb_add_error(lua_State *L)
{
    struct fb_ud *fu = check_fb(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const char *msg = luaL_checkstring(L, 3);

    if (ncb_form_builder_add_error(fu->fb, name, msg) != NCB_OK)
        return luaL_error(L, "netchan: the form has no room for more errors");
    return 0;
}

/* add_field{type=, flags=, name=, label=, value=, pattern=, min=, max=,
 * options={...}}. Returns the new field's 1-based index. */
static int
l_fb_add_field(lua_State *L)
{
    struct fb_ud *fu = check_fb(L, 1);
    lua_Number num;
    int idx;
    int type;
    int flags;
    const char *name;
    const char *label;
    const char *value;
    const char *pattern;
    int32_t min = 0;
    int32_t max = 0;
    int ranged = 0;

    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "type");
    type = luaL_checkint(L, -1);
    lua_pop(L, 1);
    flags = 0;
    if (get_field_number(L, 2, "flags", &num))
        flags = (int)num;
    name = get_field_string(L, 2, "name", "");
    label = get_field_string(L, 2, "label", "");
    value = get_field_string(L, 2, "value", NULL);
    pattern = get_field_string(L, 2, "pattern", NULL);
    idx = ncb_form_builder_add_field(fu->fb, type, flags, name, label, value,
                                     pattern);
    if (idx < 0)
        return luaL_error(L, "netchan: the form is full");
    if (get_field_number(L, 2, "min", &num)) {
        min = (int32_t)num;
        ranged = 1;
    }
    if (get_field_number(L, 2, "max", &num)) {
        max = (int32_t)num;
        ranged = 1;
    }
    if (ranged)
        ncb_form_builder_field_range(fu->fb, idx, min, max);
    lua_getfield(L, 2, "options");
    if (lua_istable(L, -1)) {
        size_t o;
        size_t count = lua_objlen(L, -1);

        for (o = 1; o <= count; o++) {
            lua_rawgeti(L, -1, (int)o);
            if (ncb_form_builder_field_option(fu->fb, idx,
                                              luaL_checkstring(L, -1))
                != NCB_OK)
                return luaL_error(L, "netchan: too many options for a field");
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_pushinteger(L, idx + 1);
    return 1;
}

/****************************************************************
 * Framing
 ****************************************************************/

static int
l_frame(lua_State *L)
{
    size_t len;
    const char *msg = luaL_checklstring(L, 1, &len);
    size_t cap = len + 2;
    uint8_t *out;
    long n;

    out = malloc(cap);
    if (!out)
        return luaL_error(L, "netchan: out of memory");
    n = ncb_auth_frame(out, cap, msg, len);
    if (n < 0) {
        free(out);
        return luaL_error(L, "netchan: message is empty or too large to frame");
    }
    lua_pushlstring(L, (const char *)out, (size_t)n);
    free(out);
    return 1;
}

static int
l_framer(lua_State *L)
{
    struct framer_ud *fu = new_handle(L, sizeof(*fu), MT_FRAMER);
    lua_Integer cap = luaL_checkinteger(L, 1);

    if (cap <= 0)
        return luaL_error(L, "netchan: framer capacity must be positive");
    fu->f = ncb_framer_new((size_t)cap);
    fu->cap = (size_t)cap;
    if (!fu->f)
        return luaL_error(L, "netchan: out of memory");
    return 1;
}

static int
l_framer_gc(lua_State *L)
{
    struct framer_ud *fu = luaL_checkudata(L, 1, MT_FRAMER);

    ncb_framer_free(fu->f);
    fu->f = NULL;
    return 0;
}

/* push(bytes) returns false when the bytes do not fit in the buffer. */
static int
l_framer_push(lua_State *L)
{
    struct framer_ud *fu = luaL_checkudata(L, 1, MT_FRAMER);
    size_t len;
    const char *bytes = luaL_checklstring(L, 2, &len);

    lua_pushboolean(L, ncb_framer_push(fu->f, bytes, len) == NCB_OK);
    return 1;
}

/* next() returns the next whole message, or nil when more bytes are needed. */
static int
l_framer_next(lua_State *L)
{
    struct framer_ud *fu = luaL_checkudata(L, 1, MT_FRAMER);
    uint8_t *out = malloc(fu->cap);
    long n;

    if (!out)
        return luaL_error(L, "netchan: out of memory");
    n = ncb_framer_next(fu->f, out, fu->cap);
    if (n < 0) {
        free(out);
        return luaL_error(L, "netchan: malformed frame in the stream");
    }
    if (n == 0)
        lua_pushnil(L);
    else
        lua_pushlstring(L, (const char *)out, (size_t)n);
    free(out);
    return 1;
}

/****************************************************************
 * Class tables
 ****************************************************************/

/* Form and form builder handles are not owned, so they need no __gc. */
static int
l_gc_none(lua_State *L)
{
    (void)L;
    return 0;
}

static const luaL_Reg conn_methods[] = {
    { "connect", l_conn_connect },
    { "accept", l_conn_accept },
    { "state", l_conn_state },
    { "id", l_conn_id },
    { "disconnect", l_conn_disconnect },
    { "close", l_conn_close },
    { "config", l_conn_config },
    { "feed", l_conn_feed },
    { "send_next", l_conn_send_next },
    { "service", l_conn_service },
    { "poll", l_conn_poll },
    { "chan_open", l_conn_chan_open },
    { "conn_stats", l_conn_stats },
    { NULL, NULL },
};

static const luaL_Reg chan_methods[] = {
    { "write", l_chan_write },
    { "read", l_chan_read },
    { "id", l_chan_id },
    { "type", l_chan_type },
    { "content_type", l_chan_content_type },
    { "state", l_chan_state },
    { "chan_stats", l_chan_stats },
    { "close", l_chan_close },
    { NULL, NULL },
};

static const luaL_Reg addr_methods[] = {
    { "text", l_addr_text },
    { NULL, NULL },
};

static const luaL_Reg cfg_methods[] = {
    { "set", l_cfg_set },
    { "get", l_cfg_get },
    { NULL, NULL },
};

static const luaL_Reg event_methods[] = {
    { "type", l_event_type },
    { "chan", l_event_chan },
    { "redirect_conn_id", l_event_redirect_conn_id },
    { "redirect_addr", l_event_redirect_addr },
    { NULL, NULL },
};

static const luaL_Reg crypto_cfg_methods[] = {
    { "set_eph_seed", l_crypto_cfg_set_eph_seed },
    { "set_psk", l_crypto_cfg_set_psk },
    { "set_static_sk", l_crypto_cfg_set_static_sk },
    { "set_require_peer_static", l_crypto_cfg_set_require_peer_static },
    { NULL, NULL },
};

static const luaL_Reg crypto_methods[] = {
    { "init", l_crypto_init },
    { "handshake_packet", l_crypto_handshake_packet },
    { "ready", l_crypto_ready },
    { "failed", l_crypto_failed },
    { "session_id", l_crypto_session_id },
    { "seal", l_crypto_seal },
    { "open", l_crypto_open },
    { "peer_static", l_crypto_peer_static },
    { NULL, NULL },
};

static const luaL_Reg beacon_methods[] = {
    { "set_game", l_beacon_set_game },
    { "game", l_beacon_game },
    { "set_wire", l_beacon_set_wire },
    { "wire", l_beacon_wire },
    { "set_version", l_beacon_set_version },
    { "major", l_beacon_major },
    { "minor", l_beacon_minor },
    { "set_port", l_beacon_set_port },
    { "port", l_beacon_port },
    { "set_players", l_beacon_set_players },
    { "players", l_beacon_players },
    { "capacity", l_beacon_capacity },
    { "set_modes", l_beacon_set_modes },
    { "modes", l_beacon_modes },
    { "set_flags", l_beacon_set_flags },
    { "flags", l_beacon_flags },
    { "set_instance", l_beacon_set_instance },
    { "instance", l_beacon_instance },
    { "set_name", l_beacon_set_name },
    { "name", l_beacon_name },
    { "set_contact", l_beacon_set_contact },
    { "contact", l_beacon_contact },
    { "set_key", l_beacon_set_key },
    { "key", l_beacon_key },
    { "build", l_beacon_build },
    { "parse", l_beacon_parse },
    { NULL, NULL },
};

static const luaL_Reg auth_methods[] = {
    { "client_init", l_auth_client_init },
    { "client_buffer", l_auth_client_buffer },
    { "start", l_auth_start },
    { "feed", l_auth_feed },
    { "needs", l_auth_needs },
    { "supply_key", l_auth_supply_key },
    { "supply_password", l_auth_supply_password },
    { "supply_method", l_auth_supply_method },
    { "offered", l_auth_offered },
    { "waiting", l_auth_waiting },
    { "registered", l_auth_registered },
    { "state", l_auth_state },
    { "user", l_auth_user },
    { "cancel", l_auth_cancel },
    { "form", l_auth_form },
    { "submit", l_auth_submit },
    { "pending_token", l_auth_pending_token },
    { "resume_pending", l_auth_resume_pending },
    { "server_init", l_auth_server_init },
    { "server_interactive", l_auth_server_interactive },
    { "server_token", l_auth_server_token },
    { "value_text", l_auth_value_text },
    { "value_int", l_auth_value_int },
    { "value_bool", l_auth_value_bool },
    { "value_choice", l_auth_value_choice },
    { NULL, NULL },
};

static const luaL_Reg form_methods[] = {
    { "title", l_form_title },
    { "note", l_form_note },
    { "nfields", l_form_nfields },
    { "field", l_form_field },
    { NULL, NULL },
};

static const luaL_Reg fb_methods[] = {
    { "title", l_fb_title },
    { "note", l_fb_note },
    { "add_field", l_fb_add_field },
    { "field_range", l_fb_field_range },
    { "field_option", l_fb_field_option },
    { "add_error", l_fb_add_error },
    { NULL, NULL },
};

static const luaL_Reg values_methods[] = {
    { "set_text", l_values_set_text },
    { "set_number", l_values_set_number },
    { "set_choice", l_values_set_choice },
    { "set_bool", l_values_set_bool },
    { NULL, NULL },
};

static const luaL_Reg framer_methods[] = {
    { "push", l_framer_push },
    { "next", l_framer_next },
    { NULL, NULL },
};

static const luaL_Reg module_funcs[] = {
    { "version", l_version },
    { "addr", l_addr },
    { "cfg", l_cfg },
    { "event", l_event },
    { "open", l_open },
    { "crypto_cfg", l_crypto_cfg },
    { "crypto", l_crypto },
    { "crypto_identity_public", l_crypto_identity_public },
    { "beacon", l_beacon },
    { "beacon_probe", l_beacon_probe },
    { "beacon_kind", l_beacon_kind },
    { "beacon_probe_game", l_beacon_probe_game },
    { "keyfile_generate", l_keyfile_generate },
    { "keyfile_load", l_keyfile_load },
    { "keyfile_encrypted", l_keyfile_encrypted },
    { "known_host", l_known_host },
    { "known_host_add", l_known_host_add },
    { "host_key", l_host_key },
    { "authorized_key", l_authorized_key },
    { "check_password", l_check_password },
    { "passwd_add", l_passwd_add },
    { "user_exists", l_user_exists },
    { "hex_encode", l_hex_encode },
    { "hex_decode", l_hex_decode },
    { "auth", l_auth },
    { "auth_signed_digest", l_auth_signed_digest },
    { "frame", l_frame },
    { "framer", l_framer },
    { "values", l_values },
    { NULL, NULL },
};

/****************************************************************
 * Constants
 ****************************************************************/

static const struct int_const module_consts[] = {
    { "RELIABLE", NETCHAN_RELIABLE },
    { "UNRELIABLE", NETCHAN_UNRELIABLE },
    { "STREAM", NETCHAN_STREAM },
    { "DIR_SEND", NETCHAN_DIR_SEND },
    { "DIR_RECV", NETCHAN_DIR_RECV },
    { "STATE_NEW", NETCHAN_STATE_NEW },
    { "STATE_CONNECTING", NETCHAN_STATE_CONNECTING },
    { "STATE_CONNECTED", NETCHAN_STATE_CONNECTED },
    { "STATE_CLOSING", NETCHAN_STATE_CLOSING },
    { "STATE_CLOSED", NETCHAN_STATE_CLOSED },
    { "EV_NONE", NETCHAN_EV_NONE },
    { "EV_CONNECTED", NETCHAN_EV_CONNECTED },
    { "EV_DISCONNECTED", NETCHAN_EV_DISCONNECTED },
    { "EV_REDIRECT", NETCHAN_EV_REDIRECT },
    { "EV_CHAN_OPEN", NETCHAN_EV_CHAN_OPEN },
    { "EV_CHAN_CLOSE", NETCHAN_EV_CHAN_CLOSE },
    { "EV_DATA", NETCHAN_EV_DATA },
    { "OK", NETCHAN_OK },
    { "ERR", NETCHAN_ERR },
    { "ERR_NOMEM", NETCHAN_ERR_NOMEM },
    { "ERR_AGAIN", NETCHAN_ERR_AGAIN },
    { "ERR_CLOSED", NETCHAN_ERR_CLOSED },
    { "ERR_FLOW", NETCHAN_ERR_FLOW },
    { "ERR_PROTO", NETCHAN_ERR_PROTO },
    { "ERR_TOOBIG", NETCHAN_ERR_TOOBIG },
    { "CRYPTO_OVERHEAD", NC_CRYPTO_OVERHEAD },
    { "CRYPTO_HELLO_LEN", NC_CRYPTO_HELLO_LEN },
    { "BEACON_ANNOUNCE", NC_BEACON_ANNOUNCE },
    { "BEACON_PROBE", NC_BEACON_PROBE },
    { "BEACON_F_CREDENTIALS", NC_BEACON_F_CREDENTIALS },
    { "BEACON_F_REGISTER", NC_BEACON_F_REGISTER },
    { "BEACON_MAX", NC_BEACON_MAX },
    { "AUTH_M_PUBKEY", NC_AUTH_M_PUBKEY },
    { "AUTH_M_PASSWORD", NC_AUTH_M_PASSWORD },
    { "AUTH_M_INTERACTIVE", NC_AUTH_M_INTERACTIVE },
    { "AUTH_M_REGISTER", NC_AUTH_M_REGISTER },
    { "AUTH_STATE_PENDING", NC_AUTH_STATE_PENDING },
    { "AUTH_STATE_OK", NC_AUTH_STATE_OK },
    { "AUTH_STATE_DENIED", NC_AUTH_STATE_DENIED },
    { "AUTH_NEED_NOTHING", NC_AUTH_NEED_NOTHING },
    { "AUTH_NEED_KEY", NC_AUTH_NEED_KEY },
    { "AUTH_NEED_PASSWORD", NC_AUTH_NEED_PASSWORD },
    { "AUTH_NEED_METHOD", NC_AUTH_NEED_METHOD },
    { "AUTH_NEED_FORM", NC_AUTH_NEED_FORM },
    { "AUTH_IA_MORE", NC_AUTH_IA_MORE },
    { "AUTH_IA_DONE", NC_AUTH_IA_DONE },
    { "AUTH_IA_DENIED", NC_AUTH_IA_DENIED },
    { "AUTH_IA_WAIT", NC_AUTH_IA_WAIT },
    { "FF_TEXT", NC_FF_TEXT },
    { "FF_PASSWORD", NC_FF_PASSWORD },
    { "FF_EMAIL", NC_FF_EMAIL },
    { "FF_INTEGER", NC_FF_INTEGER },
    { "FF_PHONE", NC_FF_PHONE },
    { "FF_BOOL", NC_FF_BOOL },
    { "FF_CHOICE", NC_FF_CHOICE },
    { "FF_NOTE", NC_FF_NOTE },
    { "FF_LINK", NC_FF_LINK },
    { "FF_REQUIRED", NC_FF_REQUIRED },
    { "FF_MULTI", NC_FF_MULTI },
    { "HOST_UNKNOWN", KS_HOST_UNKNOWN },
    { "HOST_MATCH", KS_HOST_MATCH },
    { "HOST_CHANGED", KS_HOST_CHANGED },
};

/****************************************************************
 * Module entry point
 ****************************************************************/

int
luaopen_netchan(lua_State *L)
{
    size_t i;
    size_t nconst = sizeof(module_consts) / sizeof(module_consts[0]);

    lua_newtable(L);
    luaL_register(L, NULL, module_funcs);
    for (i = 0; i < nconst; i++) {
        lua_pushinteger(L, module_consts[i].value);
        lua_setfield(L, -2, module_consts[i].name);
    }

    define_class(L, MT_CONN, conn_methods, l_conn_gc);
    define_class(L, MT_CHAN, chan_methods, l_chan_gc);
    define_class(L, MT_ADDR, addr_methods, l_addr_gc);
    define_class(L, MT_CFG, cfg_methods, l_cfg_gc);
    define_class(L, MT_EVENT, event_methods, l_event_gc);
    define_class(L, MT_CRYPTO, crypto_methods, l_crypto_gc);
    define_class(L, MT_CRYPTO_CFG, crypto_cfg_methods, l_crypto_cfg_gc);
    define_class(L, MT_BEACON, beacon_methods, l_beacon_gc);
    define_class(L, MT_AUTH, auth_methods, l_auth_gc);
    define_class(L, MT_FORM, form_methods, l_gc_none);
    define_class(L, MT_FB, fb_methods, l_gc_none);
    define_class(L, MT_VALUES, values_methods, l_values_gc);
    define_class(L, MT_FRAMER, framer_methods, l_framer_gc);
    return 1;
}

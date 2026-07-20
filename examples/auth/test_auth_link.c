/* test_auth_link.c : authenticated echo over a real loopback socket */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "iox_loop.h"
#include "iox_timer.h"
#include "auth_link.h"
#include "sockutil.h"
#include "monocypher.h"
#include "iox_fd.h"
#include <sys/socket.h>

/*
 * Both ends run on one iox loop over two real 127.0.0.1 sockets, so the
 * whole stack is exercised: the crypto handshake with an identity key,
 * netchan's connect, the login, and an application round trip. The
 * credential store is a set of in-memory callbacks, which keeps the test
 * from touching the filesystem.
 */

#define WATCHDOG_MS 5000

static uint8_t host_sk[32], host_pk[32];
static uint8_t user_sk[64], user_pk[32];
static uint8_t expect_pk[32];       /* what the client will accept */

static struct iox_loop *loop;
static int echo_seen;
static int host_refused;
static int failures;
static const char *phase;

static void
check(const char *what, int cond)
{
    printf("%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

/****************************************************************
 * Credentials, in memory
 ****************************************************************/

/* The client's answer to a credential request. A real client would go and ask
 * a human here and reply whenever it heard back; the test replies at once. */
static void
cli_need(struct auth_link *al, int what, void *user)
{
    (void)user;
    if (what == NC_AUTH_NEED_KEY)
        auth_link_supply_key(al, user_sk, user_pk);
    else
        auth_link_supply_password(al, NULL);
}

static unsigned
s_methods(void *ctx, const char *user)
{
    (void)ctx;
    (void)user;
    return NC_AUTH_M_PUBKEY;
}

static int
s_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
    (void)ctx;
    return strcmp(user, "alice") == 0 && crypto_verify32(pk, user_pk) == 0;
}

/* The known-hosts decision, reduced to a pin. */
static int
verify_host(void *ctx, const uint8_t *peer_pk)
{
    (void)ctx;
    if (!peer_pk)
        return -1;
    return crypto_verify32(peer_pk, expect_pk) == 0 ? 0 : -1;
}

/****************************************************************
 * Session callbacks
 ****************************************************************/

static void
cli_up(struct auth_link *al, void *user)
{
    (void)user;
    auth_link_send(al, "ping", 4);
}

static void
cli_data(struct auth_link *al, const uint8_t *data, size_t len, void *user)
{
    (void)al;
    (void)user;
    if (len == 4 && memcmp(data, "ping", 4) == 0)
        echo_seen = 1;
    iox_loop_stop(loop);
}

static void
cli_down(struct auth_link *al, int reason, void *user)
{
    (void)al;
    (void)user;
    if (reason == AL_DOWN_HOSTKEY)
        host_refused = 1;
    iox_loop_stop(loop);
}

static void
srv_data(struct auth_link *al, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    auth_link_send(al, data, len);
}

static void
watchdog(struct iox_loop *l, void *arg)
{
    (void)arg;
    printf("FAIL: %s timed out\n", phase);
    failures++;
    iox_loop_stop(l);
}

/****************************************************************
 * One run of the whole stack
 ****************************************************************/

static int
run_once(const char *what)
{
    struct auth_link_cfg scfg, ccfg;
    struct auth_link_cb scb, ccb;
    struct auth_link *server, *client;
    struct nc_addr peer;
    int sfd, cfd, port, wd;

    phase = what;
    echo_seen = 0;
    host_refused = 0;

    sfd = su_udp_bind("127.0.0.1", 0);
    cfd = su_udp_bind("127.0.0.1", 0);
    if (sfd < 0 || cfd < 0) {
        printf("FAIL: cannot bind loopback sockets\n");
        failures++;
        return -1;
    }
    port = su_local_port(sfd);
    if (port < 0 || su_resolve("127.0.0.1", port, &peer) != 0) {
        printf("FAIL: cannot address the server socket\n");
        failures++;
        close(sfd);
        close(cfd);
        return -1;
    }

    loop = iox_loop_new();

    memset(&scfg, 0, sizeof(scfg));
    scfg.server = 1;
    scfg.static_sk = host_sk;
    scfg.scb.methods = s_methods;
    scfg.scb.check_key = s_check_key;
    memset(&scb, 0, sizeof(scb));
    scb.on_data = srv_data;

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.server = 0;
    ccfg.peer = &peer;
    ccfg.user = "alice";
    ccfg.require_peer_static = 1;
    ccfg.verify_peer = verify_host;
    memset(&ccb, 0, sizeof(ccb));
    ccb.on_up = cli_up;
    ccb.on_data = cli_data;
    ccb.on_down = cli_down;
    ccb.on_need = cli_need;

    server = auth_link_open(loop, sfd, &scfg, &scb);
    client = auth_link_open(loop, cfd, &ccfg, &ccb);
    if (!server || !client) {
        printf("FAIL: cannot open the links\n");
        failures++;
        iox_loop_free(loop);
        close(sfd);
        close(cfd);
        return -1;
    }

    wd = iox_timer_add(loop, WATCHDOG_MS, watchdog, NULL);
    iox_loop_run(loop);
    iox_timer_remove(loop, wd);

    auth_link_close(client);
    auth_link_close(server);
    iox_loop_free(loop);
    close(sfd);
    close(cfd);
    return 0;
}

/****************************************************************
 * The tick has to keep ticking
 ****************************************************************/

static int hello_count;

/* A bare socket that answers nothing, so the only thing that can produce a
 * second datagram is the client's own periodic timer. */
static void
count_datagrams(struct iox_loop *l, int fd, unsigned events, void *arg)
{
    uint8_t buf[2048];

    (void)l;
    (void)events;
    (void)arg;
    while (recv(fd, buf, sizeof(buf), 0) > 0)
        hello_count++;
}

static void
stop_now(struct iox_loop *l, void *arg)
{
    (void)arg;
    iox_loop_stop(l);
}

/*
 * A client whose peer never replies must go on repeating its HELLO. This is
 * the regression test for a timer that was scheduled once and then quietly
 * retired, which left every session with no clock: no retransmissions, no
 * repeated handshake, and no way to notice a peer that had gone away. The
 * failure is invisible in any test where both ends answer promptly.
 */
static void
test_tick_keeps_running(void)
{
    struct auth_link_cfg ccfg;
    struct auth_link_cb ccb;
    struct auth_link *client;
    struct nc_addr peer;
    int sfd, cfd, port, wd;

    hello_count = 0;
    sfd = su_udp_bind("127.0.0.1", 0);
    cfd = su_udp_bind("127.0.0.1", 0);
    if (sfd < 0 || cfd < 0) {
        printf("FAIL: cannot bind loopback sockets\n");
        failures++;
        return;
    }
    port = su_local_port(sfd);
    if (port < 0 || su_resolve("127.0.0.1", port, &peer) != 0) {
        printf("FAIL: cannot address the silent socket\n");
        failures++;
        close(sfd);
        close(cfd);
        return;
    }

    loop = iox_loop_new();
    iox_fd_add(loop, sfd, IOX_READ, count_datagrams, NULL);

    memset(&ccfg, 0, sizeof(ccfg));
    ccfg.server = 0;
    ccfg.peer = &peer;
    ccfg.user = "alice";
    ccfg.require_peer_static = 1;
    ccfg.verify_peer = verify_host;
    memset(&ccb, 0, sizeof(ccb));
    ccb.on_need = cli_need;

    client = auth_link_open(loop, cfd, &ccfg, &ccb);
    wd = iox_timer_add(loop, 400, stop_now, NULL);
    iox_loop_run(loop);
    iox_timer_remove(loop, wd);

    /* 400ms at a 50ms tick is about eight chances to repeat. Anything past
     * the first datagram proves the timer survived its own first firing. */
    check("a silent peer gets the handshake repeated", hello_count >= 3);

    auth_link_close(client);
    iox_fd_remove(loop, sfd);
    iox_loop_free(loop);
    close(sfd);
    close(cfd);
}

int
main(void)
{
    uint8_t seed[32];

    memset(seed, 7, sizeof(seed));
    memcpy(host_sk, seed, 32);
    nc_crypto_identity_public(host_pk, host_sk);

    memset(seed, 9, sizeof(seed));
    crypto_eddsa_key_pair(user_sk, user_pk, seed);

    /* 1. The good path: the pinned host key matches and the user's key is
     *    authorised, so an application round trip completes. */
    memcpy(expect_pk, host_pk, 32);
    if (run_once("authenticated echo") == 0)
        check("encrypted authenticated echo round-trip", echo_seen);

    /* 2. The host key is not the one on file. The client must refuse before
     *    it ever sends a credential. */
    memset(expect_pk, 0x55, sizeof(expect_pk));
    if (run_once("host key mismatch") == 0) {
        check("wrong host key is refused", host_refused);
        check("nothing was echoed to a rejected host", !echo_seen);
    }

    /* 3. The periodic tick must outlive its first firing. */
    phase = "tick";
    test_tick_keeps_running();

    if (failures)
        printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

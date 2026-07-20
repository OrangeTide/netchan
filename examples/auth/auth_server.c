/* auth_server.c : echo server that authenticates its clients */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "iox_loop.h"
#include "iox_signal.h"
#include "auth_link.h"
#include "keystore.h"
#include "prompt.h"
#include "sockutil.h"

struct server {
    struct iox_loop  *loop;
    struct auth_link *link;
    const char       *authkeys;
    const char       *passwd;
};

/****************************************************************
 * The credential store, as three callbacks
 ****************************************************************/

/*
 * Offer the same methods for every name, known or not. Tailoring the answer
 * to whether the account exists would turn this one message into an account
 * enumerator, and the client learns nothing by trying and failing.
 */
static unsigned
srv_methods(void *ctx, const char *user)
{
    (void)ctx;
    (void)user;
    return NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD;
}

static int
srv_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
    struct server *s = ctx;

    return ks_authorized_key(s->authkeys, user, pk);
}

static int
srv_check_password(void *ctx, const char *user, const char *password)
{
    struct server *s = ctx;

    return ks_check_password(s->passwd, user, password);
}

/****************************************************************
 * Session callbacks
 ****************************************************************/

static void
on_up(struct auth_link *al, void *user)
{
    (void)user;
    printf("* %s authenticated\n", auth_link_user(al));
    fflush(stdout);
}

static void
on_data(struct auth_link *al, const uint8_t *data, size_t len, void *user)
{
    (void)user;
    printf("< %.*s\n", (int)len, (const char *)data);
    fflush(stdout);
    auth_link_send(al, data, len);
}

static void
on_down(struct auth_link *al, int reason, void *user)
{
    struct server *s = user;

    (void)al;
    switch (reason) {
    case AL_DOWN_AUTH:
        printf("* login refused\n");
        break;
    case AL_DOWN_HOSTKEY:
        printf("* peer identity refused\n");
        break;
    default:
        printf("* client disconnected\n");
        break;
    }
    fflush(stdout);
    iox_loop_stop(s->loop);
}

static void
on_signal(struct iox_loop *loop, int signo, void *arg)
{
    (void)signo;
    (void)arg;
    printf("\n* shutting down\n");
    iox_loop_stop(loop);
}

/****************************************************************
 * main
 ****************************************************************/

static void
usage(void)
{
    fprintf(stderr,
        "usage: auth_server [--port N] [--hostkey F] [--authkeys F]\n"
        "                   [--passwd F] [--adduser NAME]\n");
    exit(2);
}

/* Enrol a password for NAME and exit, so the demo needs no separate tool. */
static int
add_user(const char *passwd_path, const char *user)
{
    char pass[NC_AUTH_MAX_PASS], again[NC_AUTH_MAX_PASS];

    if (ks_user_exists(passwd_path, user)) {
        fprintf(stderr, "auth_server: %s already has a password entry\n", user);
        return 1;
    }
    if (prompt_hidden("New password: ", pass, sizeof(pass)) != 0)
        return 1;
    if (prompt_hidden("Same again: ", again, sizeof(again)) != 0)
        return 1;
    if (strcmp(pass, again) != 0) {
        fprintf(stderr, "auth_server: passwords differ\n");
        return 1;
    }
    if (ks_passwd_add(passwd_path, user, pass) != 0) {
        fprintf(stderr, "auth_server: cannot write %s\n", passwd_path);
        return 1;
    }
    printf("added %s to %s\n", user, passwd_path);
    return 0;
}

int
main(int argc, char **argv)
{
    struct server s;
    struct auth_link_cfg cfg;
    struct auth_link_cb cb;
    const char *hostkey_path = "host_key";
    const char *adduser = NULL;
    uint8_t host_sk[32], host_pk[32];
    char hex[65];
    int port = 9000;
    int fd;

    memset(&s, 0, sizeof(s));
    s.authkeys = "authorized_keys";
    s.passwd = "passwd";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--hostkey") == 0 && i + 1 < argc)
            hostkey_path = argv[++i];
        else if (strcmp(argv[i], "--authkeys") == 0 && i + 1 < argc)
            s.authkeys = argv[++i];
        else if (strcmp(argv[i], "--passwd") == 0 && i + 1 < argc)
            s.passwd = argv[++i];
        else if (strcmp(argv[i], "--adduser") == 0 && i + 1 < argc)
            adduser = argv[++i];
        else
            usage();
    }

    if (adduser)
        return add_user(s.passwd, adduser);

    /* The identity key is created on first run and never changes after,
     * which is exactly the property a client's known_hosts entry depends
     * on. Losing this file is what produces the frightening warning. */
    if (ks_host_key(hostkey_path, host_sk) != 0) {
        fprintf(stderr, "auth_server: cannot load or create %s\n", hostkey_path);
        return 1;
    }
    nc_crypto_identity_public(host_pk, host_sk);
    ks_hex_encode(hex, host_pk, sizeof(host_pk));

    fd = su_udp_bind(NULL, port);
    if (fd < 0) {
        fprintf(stderr, "auth_server: cannot bind udp/%d\n", port);
        return 1;
    }

    s.loop = iox_loop_new();
    if (!s.loop) {
        close(fd);
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.server = 1;
    cfg.static_sk = host_sk;
    cfg.scb.methods = srv_methods;
    cfg.scb.check_key = srv_check_key;
    cfg.scb.check_password = srv_check_password;
    cfg.scb.ctx = &s;

    memset(&cb, 0, sizeof(cb));
    cb.on_up = on_up;
    cb.on_data = on_data;
    cb.on_down = on_down;
    cb.user = &s;

    s.link = auth_link_open(s.loop, fd, &cfg, &cb);
    if (!s.link) {
        fprintf(stderr, "auth_server: cannot start session\n");
        iox_loop_free(s.loop);
        close(fd);
        return 1;
    }

    iox_signal_add(s.loop, SIGINT, on_signal, &s);

    printf("listening on udp/%d\n", port);
    printf("host key %s\n", hex);
    fflush(stdout);

    iox_loop_run(s.loop);

    auth_link_close(s.link);
    iox_loop_free(s.loop);
    close(fd);
    return 0;
}

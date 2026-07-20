/* auth_client.c : echo client that checks the host key and logs in */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "iox_loop.h"
#include "iox_fd.h"
#include "iox_signal.h"
#include "iox_timer.h"
#include "auth_link.h"
#include "keystore.h"
#include "prompt.h"
#include "sockutil.h"

/* How long to keep the loop alive after stdin closes, so echoes still in
 * flight have time to come back before the process exits. */
#define LINGER_MS 400

struct client {
    struct iox_loop  *loop;
    struct auth_link *link;
    const char       *host;
    const char       *known_hosts;
    const char       *keyfile;
    const char       *user;
    int               use_key;
    int               interactive;      /* stdin is a terminal */
    int               stdin_watched;
    int               asking;           /* NC_AUTH_NEED_* being collected */
    int               denied;
    struct prompt_reader reader;
};

/****************************************************************
 * Trust on first use
 ****************************************************************/

/*
 * The whole known-hosts decision, in one callback. nc_crypto has already
 * established that whoever holds this key will be the only party able to
 * seal packets we can open; the only question left is whether this is the
 * key we saw last time.
 *
 * An unknown host is recorded and accepted, which is ssh's accept-new
 * behaviour rather than its default prompt. It leaves exactly one window of
 * exposure, the first connection, and it is the honest cost of having no
 * certificate authority. A changed key is refused outright, because at that
 * point the alternatives are a reinstalled server or an impostor, and only
 * the operator can tell which.
 */
static int
verify_host(void *ctx, const uint8_t *peer_pk)
{
    struct client *c = ctx;
    uint8_t stored[32];
    char hex[65], old_hex[65];

    if (!peer_pk) {
        fprintf(stderr, "client: server presented no identity key\n");
        return -1;
    }
    ks_hex_encode(hex, peer_pk, 32);

    switch (ks_known_host(c->known_hosts, c->host, peer_pk, stored)) {
    case KS_HOST_MATCH:
        return 0;

    case KS_HOST_UNKNOWN:
        printf("* unknown host %s, key %s\n", c->host, hex);
        printf("* permanently added to %s\n", c->known_hosts);
        fflush(stdout);
        ks_known_host_add(c->known_hosts, c->host, peer_pk);
        return 0;

    default:
        ks_hex_encode(old_hex, stored, 32);
        fprintf(stderr,
            "\n"
            "  WARNING: THE HOST IDENTIFICATION HAS CHANGED\n"
            "  Someone could be eavesdropping on you right now.\n"
            "  The key for %s in %s is\n"
            "    %s\n"
            "  but the server presented\n"
            "    %s\n"
            "  Remove the stale line if you know why it changed.\n\n",
            c->host, c->known_hosts, old_hex, hex);
        return -1;
    }
}

/****************************************************************
 * Credentials
 ****************************************************************/

/*
 * Answering a credential request never blocks. A key file that needs no
 * passphrase is answered on the spot; anything that needs a human turns into
 * a prompt plus a watcher on stdin, and the loop goes on servicing the
 * connection while the user types.
 */

static void on_secret_typed(struct iox_loop *loop, int fd,
                            unsigned events, void *arg);

/* Start collecting a hidden line. what_for says which supply call the
 * completed line belongs to. */
static void
ask_secret(struct client *c, int what_for, const char *prompt)
{
    c->asking = what_for;
    prompt_reader_begin(&c->reader, prompt);
    if (iox_fd_add(c->loop, STDIN_FILENO, IOX_READ, on_secret_typed, c) != 0) {
        prompt_reader_end(&c->reader);
        c->asking = 0;
        /* Cannot ask, so decline and let the login try something else. */
        if (what_for == NC_AUTH_NEED_KEY)
            auth_link_supply_key(c->link, NULL, NULL);
        else
            auth_link_supply_password(c->link, NULL);
        return;
    }
    c->stdin_watched = 1;
}

/* Load the key file and hand it over, prompting first if it is sealed. */
static void
supply_key_from_file(struct client *c)
{
    uint8_t sk[64], pk[32];

    if (!c->use_key || access(c->keyfile, R_OK) != 0) {
        auth_link_supply_key(c->link, NULL, NULL);   /* none enrolled */
        return;
    }
    if (ks_keyfile_encrypted(c->keyfile)) {
        ask_secret(c, NC_AUTH_NEED_KEY, "Key passphrase: ");
        return;
    }
    if (ks_keyfile_load(c->keyfile, NULL, sk, pk) != 0) {
        fprintf(stderr, "client: cannot read %s\n", c->keyfile);
        auth_link_supply_key(c->link, NULL, NULL);
        return;
    }
    auth_link_supply_key(c->link, sk, pk);
    memset(sk, 0, sizeof(sk));
}

/* A complete hidden line has arrived. */
static void
secret_complete(struct client *c)
{
    uint8_t sk[64], pk[32];
    int what = c->asking;
    int rc;

    iox_fd_remove(c->loop, STDIN_FILENO);
    c->stdin_watched = 0;
    c->asking = 0;

    if (what == NC_AUTH_NEED_PASSWORD) {
        auth_link_supply_password(c->link, c->reader.buf);
        prompt_reader_end(&c->reader);
        return;
    }

    rc = ks_keyfile_load(c->keyfile, c->reader.buf, sk, pk);
    prompt_reader_end(&c->reader);
    if (rc == -2)
        fprintf(stderr, "client: wrong passphrase for %s\n", c->keyfile);
    else if (rc != 0)
        fprintf(stderr, "client: cannot read %s\n", c->keyfile);

    if (rc == 0) {
        auth_link_supply_key(c->link, sk, pk);
        memset(sk, 0, sizeof(sk));
    } else {
        auth_link_supply_key(c->link, NULL, NULL);
    }
}

static void
on_secret_typed(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    struct client *c = arg;
    int r;

    (void)loop;
    (void)events;

    r = prompt_reader_feed(&c->reader, fd);
    if (r == 0)
        return;             /* still typing */
    if (r < 0) {
        int what = c->asking;

        iox_fd_remove(c->loop, STDIN_FILENO);
        c->stdin_watched = 0;
        c->asking = 0;
        prompt_reader_end(&c->reader);
        if (what == NC_AUTH_NEED_KEY)
            auth_link_supply_key(c->link, NULL, NULL);
        else
            auth_link_supply_password(c->link, NULL);
        return;
    }
    secret_complete(c);
}

static void
on_need(struct auth_link *al, int what, void *user)
{
    struct client *c = user;
    char prompt[160];

    (void)al;
    if (what == NC_AUTH_NEED_KEY) {
        supply_key_from_file(c);
        return;
    }
    snprintf(prompt, sizeof(prompt), "%s's password: ", c->user);
    ask_secret(c, NC_AUTH_NEED_PASSWORD, prompt);
}

/****************************************************************
 * Session callbacks
 ****************************************************************/

static void on_stdin(struct iox_loop *loop, int fd, unsigned events, void *arg);

/*
 * Draw the prompt, but only on a terminal. Under a pipe it would interleave
 * with the output the tests and the screenshots read back.
 *
 * The terminal stays in canonical mode, so the kernel's line discipline is
 * already providing backspace and the other line-editing keys, and each read
 * delivers a whole line. The prompt goes to stderr so stdout carries only the
 * session's own output. An echo that lands while a line is half typed will
 * appear in the middle of it; fixing that properly means raw mode and
 * redrawing the input, which is a line editor rather than a prompt.
 */
static void
draw_prompt(struct client *c)
{
    if (!c->interactive)
        return;
    fputs("netchan> ", stderr);
    fflush(stderr);
}

static void
on_up(struct auth_link *al, void *user)
{
    struct client *c = user;

    (void)al;
    printf("* authenticated as %s, type to echo\n", auth_link_user(al));
    fflush(stdout);

    /* Only now start reading the keyboard, so a line can never be typed
     * before there is an authenticated channel to carry it. */
    if (iox_fd_add(c->loop, STDIN_FILENO, IOX_READ, on_stdin, c) == 0)
        c->stdin_watched = 1;
    draw_prompt(c);
}

static void
on_data(struct auth_link *al, const uint8_t *data, size_t len, void *user)
{
    struct client *c = user;

    (void)al;
    printf("[echo] %.*s\n", (int)len, (const char *)data);
    fflush(stdout);
    draw_prompt(c);
}

static void
on_down(struct auth_link *al, int reason, void *user)
{
    struct client *c = user;

    (void)al;
    if (c->interactive && c->stdin_watched)
        fputc('\n', stderr);            /* the prompt is still on screen */
    switch (reason) {
    case AL_DOWN_AUTH:
        fprintf(stderr, "client: authentication failed\n");
        c->denied = 1;
        break;
    case AL_DOWN_HOSTKEY:
        fprintf(stderr, "client: host key not accepted\n");
        c->denied = 1;
        break;
    default:
        printf("* server closed the session\n");
        break;
    }
    iox_loop_stop(c->loop);
}

static void
linger_done(struct iox_loop *loop, void *arg)
{
    (void)arg;
    iox_loop_stop(loop);
}

static void
on_stdin(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
    struct client *c = arg;
    char line[1024];
    ssize_t n;

    (void)loop;
    (void)events;

    n = read(fd, line, sizeof(line));
    if (n <= 0) {
        /* Stop reading, but give in-flight echoes a moment to arrive. */
        if (c->interactive)
            fputc('\n', stderr);        /* close off the dangling prompt */
        iox_fd_remove(c->loop, STDIN_FILENO);
        c->stdin_watched = 0;
        iox_timer_add(c->loop, LINGER_MS, linger_done, c);
        return;
    }

    /* One line per message; netchan keeps the boundaries. */
    char *p = line, *end = line + n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

        if (len > 0)
            auth_link_send(c->link, p, len);
        p += len + (nl ? 1 : 0);
    }
}

static void
on_signal(struct iox_loop *loop, int signo, void *arg)
{
    (void)signo;
    (void)arg;
    iox_loop_stop(loop);
}

/****************************************************************
 * main
 ****************************************************************/

static void
usage(void)
{
    fprintf(stderr,
        "usage: auth_client [--host H] [--port N] [--user NAME]\n"
        "                   [--key F] [--known-hosts F] [--password]\n");
    exit(2);
}

int
main(int argc, char **argv)
{
    struct client c;
    struct auth_link_cfg cfg;
    struct auth_link_cb cb;
    struct nc_addr peer;
    const char *env_user = getenv("USER");
    int port = 9000;
    int fd, rc = 0;

    memset(&c, 0, sizeof(c));
    c.host = "127.0.0.1";
    c.known_hosts = "known_hosts";
    c.keyfile = "id_netchan";
    c.user = env_user ? env_user : "player";
    c.use_key = 1;
    c.interactive = isatty(STDIN_FILENO);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            c.host = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc)
            c.user = argv[++i];
        else if (strcmp(argv[i], "--key") == 0 && i + 1 < argc)
            c.keyfile = argv[++i];
        else if (strcmp(argv[i], "--known-hosts") == 0 && i + 1 < argc)
            c.known_hosts = argv[++i];
        else if (strcmp(argv[i], "--password") == 0)
            c.use_key = 0;
        else
            usage();
    }

    if (su_resolve(c.host, port, &peer) != 0) {
        fprintf(stderr, "client: cannot resolve %s\n", c.host);
        return 1;
    }
    fd = su_udp_bind(NULL, 0);
    if (fd < 0) {
        fprintf(stderr, "client: cannot open a socket\n");
        return 1;
    }

    c.loop = iox_loop_new();
    if (!c.loop) {
        close(fd);
        return 1;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.server = 0;
    cfg.peer = &peer;
    cfg.user = c.user;
    /* An anonymous server cannot be recognised on a later visit, so refuse
     * one outright rather than silently downgrading to encryption only. */
    cfg.require_peer_static = 1;
    cfg.verify_peer = verify_host;
    cfg.verify_ctx = &c;

    memset(&cb, 0, sizeof(cb));
    cb.on_up = on_up;
    cb.on_data = on_data;
    cb.on_down = on_down;
    cb.on_need = on_need;
    cb.user = &c;

    c.link = auth_link_open(c.loop, fd, &cfg, &cb);
    if (!c.link) {
        fprintf(stderr, "client: cannot start session\n");
        iox_loop_free(c.loop);
        close(fd);
        return 1;
    }

    iox_signal_add(c.loop, SIGINT, on_signal, &c);

    printf("connecting to %s:%d as %s\n", c.host, port, c.user);
    fflush(stdout);

    iox_loop_run(c.loop);

    if (c.stdin_watched)
        iox_fd_remove(c.loop, STDIN_FILENO);
    rc = c.denied ? 1 : 0;
    auth_link_close(c.link);
    iox_loop_free(c.loop);
    close(fd);
    return rc;
}

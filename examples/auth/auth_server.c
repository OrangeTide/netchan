/* auth_server.c : echo server that authenticates its clients */

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
 *
 * Registration is offered on the same terms, to everyone, which is what keeps
 * that property. The form does leak: it has to say when a name is taken. But
 * it leaks to somebody who asked for a form, rather than to everybody who
 * connects.
 */
static unsigned
srv_methods(void *ctx, const char *user)
{
    (void)ctx;
    (void)user;
    return NC_AUTH_M_PUBKEY | NC_AUTH_M_PASSWORD | NC_AUTH_M_REGISTER;
}

static bool
srv_check_key(void *ctx, const char *user, const uint8_t pk[32])
{
    struct server *s = ctx;

    return ks_authorized_key(s->authkeys, user, pk);
}

static bool
srv_check_password(void *ctx, const char *user, const char *password)
{
    struct server *s = ctx;

    return ks_check_password(s->passwd, user, password);
}

/****************************************************************
 * Registration, as a form
 *
 * The server states the form; nothing in netchan parses a config file or
 * knows what any of these fields mean. A real operator would read this out
 * of their own configuration, which is the whole reason the format describes
 * itself: a client compiled a year ago renders a field added last week.
 ****************************************************************/

static const struct nc_form_field REGISTER_FIELDS[] = {
    { NC_FF_TEXT, NC_FF_REQUIRED, "user", "Account name", NULL,
      NULL, NULL, 20, NC_AUTH_MAX_USER, 0, 0, NULL, 0 },
    { NC_FF_PASSWORD, NC_FF_REQUIRED, "pw", "Password", NULL,
      NULL, NULL, 20, NC_AUTH_MAX_PASS - 1, 0, 0, NULL, 0 },
    { NC_FF_PASSWORD, NC_FF_REQUIRED, "pw2", "Password again", NULL,
      NULL, NULL, 20, NC_AUTH_MAX_PASS - 1, 0, 0, NULL, 0 },
    { NC_FF_NOTE, 0, NULL,
      "The password is stored as an Argon2id hash, never as itself.",
      NULL, NULL, NULL, 0, 0, 0, 0, NULL, 0 },
};

static void
registration_form(struct nc_form *out, const struct nc_form_error *errs, int n)
{
    memset(out, 0, sizeof(*out));
    out->title = "Create an account";
    out->fields = REGISTER_FIELDS;
    out->nfields = (uint8_t)(sizeof(REGISTER_FIELDS) /
                             sizeof(REGISTER_FIELDS[0]));
    out->errors = errs;
    out->nerrors = (uint8_t)n;
}

static int
srv_begin(void *ctx, unsigned method, struct nc_form *out)
{
    (void)ctx;
    (void)method;
    registration_form(out, NULL, 0);
    return NC_AUTH_IA_MORE;
}

/*
 * Everything the client sent is a hint until it is checked here. The form
 * said the name is required and gave a maximum length, and none of that
 * binds a client that chose to ignore it.
 */
static int
srv_submit(void *ctx, struct nc_auth *a, struct nc_form *out)
{
    static struct nc_form_error errs[3];
    struct server *s = ctx;
    const char *user = nc_auth_value_text(a, "user");
    const char *pw = nc_auth_value_text(a, "pw");
    const char *pw2 = nc_auth_value_text(a, "pw2");
    int n = 0;

    if (!user || user[0] == '\0' || strlen(user) > NC_AUTH_MAX_USER) {
        errs[n].name = "user";
        errs[n].message = "a name is required";
        n++;
    } else if (ks_user_exists(s->passwd, user)) {
        errs[n].name = "user";
        errs[n].message = "that name is taken";
        n++;
    }
    if (!pw || strlen(pw) < 8) {
        errs[n].name = "pw";
        errs[n].message = "at least eight characters";
        n++;
    } else if (!pw2 || strcmp(pw, pw2) != 0) {
        errs[n].name = "pw2";
        errs[n].message = "the two do not match";
        n++;
    }
    if (n > 0) {
        registration_form(out, errs, n);
        return NC_AUTH_IA_MORE;      /* ask again, with the boxes marked */
    }

    if (ks_passwd_add(s->passwd, user, pw) != 0) {
        errs[0].name = "user";
        errs[0].message = "the server could not save that";
        registration_form(out, errs, 1);
        return NC_AUTH_IA_MORE;
    }
    printf("* registered %s\n", user);
    fflush(stdout);

    /*
     * Done, and deliberately not logged in. The conversation goes back to
     * the method offer, the methods callback runs again, and the client
     * authenticates with the credential it just enrolled. One path into a
     * session, the same one a returning player takes.
     */
    return NC_AUTH_IA_DONE;
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
    cfg.iacb.begin = srv_begin;
    cfg.iacb.submit = srv_submit;
    cfg.iacb.ctx = &s;

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

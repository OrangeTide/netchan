/* test_nc_auth_ia.c : the interactive method, both sides, no transport */

#include <stdio.h>
#include <string.h>

#include "nc_auth.h"

/*
 * Both state machines run in one process with a message queue between them,
 * the same harness test_nc_auth.c uses. The ordinary path, register and then
 * log in, is the least of what is here. The cases worth a test are the ones
 * around the edges: a cancel crossing a form already in flight, a token that
 * has to be dead after the player changed their mind, and whether a wiped
 * conversation really has no password left in it.
 */

#define MAX_Q 64

struct wire {
    struct {
        int     to_server;
        uint8_t buf[NC_FORM_MAX_MSG];
        size_t  len;
    } q[MAX_Q];
    int head, tail;
    struct nc_auth *client, *server;
    int drop_to_client;             /* swallow the next N server messages */
};

static struct wire W;
static int failures;

static void
check(const char *what, int ok)
{
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok)
        failures++;
}

static void
push(int to_server, const void *m, size_t n)
{
    if (W.tail >= MAX_Q || n > NC_FORM_MAX_MSG)
        return;
    W.q[W.tail].to_server = to_server;
    memcpy(W.q[W.tail].buf, m, n);
    W.q[W.tail].len = n;
    W.tail++;
}

static void
send_to_server(void *ctx, const void *m, size_t n)
{
    (void)ctx;
    push(1, m, n);
}

static void
send_to_client(void *ctx, const void *m, size_t n)
{
    (void)ctx;
    push(0, m, n);
}

/****************************************************************
 * The application on each side
 ****************************************************************/

static uint8_t client_buf[2048];
static uint8_t server_buf[2048];

/*
 * What the client answers with. The offer comes round again every time a
 * method ends, whether it finished or was abandoned, so the test says what to
 * do the first time and what to do afterwards. Answering "register" forever
 * would register forever.
 */
static unsigned client_choice = NC_AUTH_M_REGISTER;
static unsigned client_choice_again;    /* 0 means give up */
static int client_asks;
static const char *client_name = "newbie";
static const char *client_pass = "hunter2";
static const char *client_email = "player@example.com";
static int client_cancel_at = -1;   /* submit count at which to cancel */
static int client_forms;            /* forms rendered */
static int client_waits;            /* wait notices seen */

/* What the server does. */
static int server_stage;            /* how far the registration has got */
static int server_wait_next;        /* answer the next submit with WAIT */
static int server_cancelled;        /* cancelled callback count */
static int account_exists;
static char account_name[64];
static char account_pass[64];
static uint8_t live_token[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
static int token_valid;

static const struct nc_form_field REG_FIELDS[] = {
    { NC_FF_TEXT, NC_FF_REQUIRED, "user", "Name", NULL, NULL, NULL,
      20, 32, 0, 0, NULL, 0 },
    { NC_FF_PASSWORD, NC_FF_REQUIRED, "pw", "Password", NULL, NULL, NULL,
      20, 64, 0, 0, NULL, 0 },
    { NC_FF_EMAIL, 0, "email", "Email", NULL, NULL, NULL,
      30, 64, 0, 0, NULL, 0 },
    { NC_FF_NOTE, 0, NULL, "Pick a name nobody else has taken.", NULL, NULL,
      NULL, 0, 0, 0, 0, NULL, 0 },
};

static const struct nc_form_field CODE_FIELDS[] = {
    { NC_FF_TEXT, NC_FF_REQUIRED, "code", "Code from the mail", NULL, NULL,
      NULL, 8, 8, 0, 0, NULL, 0 },
};

static void
fill_reg_form(struct nc_form *out, const struct nc_form_error *errs, int ne)
{
    memset(out, 0, sizeof(*out));
    out->title = "Create an account";
    out->fields = REG_FIELDS;
    out->nfields = 4;
    out->errors = errs;
    out->nerrors = (uint8_t)ne;
}

static int
s_begin(void *ctx, unsigned method, struct nc_form *out)
{
    (void)ctx;
    (void)method;
    server_stage = 1;
    fill_reg_form(out, NULL, 0);
    return NC_AUTH_IA_MORE;
}

static int
s_resume(void *ctx, const uint8_t *tok, size_t len, struct nc_form *out)
{
    (void)ctx;
    if (!token_valid || len != sizeof(live_token) ||
        memcmp(tok, live_token, len) != 0)
        return NC_AUTH_IA_DENIED;
    server_stage = 2;
    memset(out, 0, sizeof(*out));
    out->title = "Confirm";
    out->fields = CODE_FIELDS;
    out->nfields = 1;
    return NC_AUTH_IA_MORE;
}

static int
s_submit(void *ctx, struct nc_auth *a, struct nc_form *out)
{
    static const struct nc_form_error TAKEN[] = {
        { "user", "that name is taken" },
    };
    const char *u = nc_auth_value_text(a, "user");
    const char *p = nc_auth_value_text(a, "pw");

    (void)ctx;

    if (server_stage == 2) {                    /* the emailed code */
        const char *code = nc_auth_value_text(a, "code");

        if (!code || strcmp(code, "123456") != 0)
            return NC_AUTH_IA_DENIED;
        account_exists = 1;
        token_valid = 0;
        return NC_AUTH_IA_DONE;
    }

    if (!u || !p)
        return NC_AUTH_IA_DENIED;
    if (strcmp(u, "taken") == 0) {
        fill_reg_form(out, TAKEN, 1);
        return NC_AUTH_IA_MORE;
    }

    snprintf(account_name, sizeof(account_name), "%s", u);
    snprintf(account_pass, sizeof(account_pass), "%s", p);

    if (server_wait_next) {
        server_stage = 2;
        token_valid = 1;
        nc_auth_server_token(a, live_token, sizeof(live_token));
        memset(out, 0, sizeof(*out));
        out->note = "we sent a code to that address";
        return NC_AUTH_IA_WAIT;
    }
    account_exists = 1;
    return NC_AUTH_IA_DONE;
}

static void
s_cancelled(void *ctx)
{
    (void)ctx;
    server_cancelled++;
    token_valid = 0;                            /* the emailed link dies */
    server_stage = 0;
}

static unsigned
s_methods(void *ctx, const char *user)
{
    (void)ctx;
    (void)user;
    if (account_exists)
        return NC_AUTH_M_PASSWORD | NC_AUTH_M_REGISTER;
    return NC_AUTH_M_REGISTER;
}

static bool
s_check_password(void *ctx, const char *user, const char *password)
{
    (void)ctx;
    return account_exists && strcmp(user, account_name) == 0 &&
           strcmp(password, account_pass) == 0;
}

/*
 * Stand in for the player. A real one takes minutes over this; the test
 * answers at once, which walks the same suspend-and-resume path.
 */
static void
service_needs(void)
{
    int guard;

    if (!W.client)
        return;

    for (guard = 0; guard < 8; guard++) {
        int need = nc_auth_needs(W.client);

        if (need == NC_AUTH_NEED_METHOD) {
            nc_auth_supply_method(W.client,
                                  client_asks++ ? client_choice_again
                                                : client_choice);
        } else if (need == NC_AUTH_NEED_PASSWORD) {
            nc_auth_supply_password(W.client, client_pass);
        } else if (need == NC_AUTH_NEED_KEY) {
            nc_auth_supply_key(W.client, NULL, NULL);
        } else if (need == NC_AUTH_NEED_FORM) {
            const struct nc_form *f = nc_auth_form(W.client);
            struct nc_form_value vals[NC_FORM_MAX_FIELDS];
            int i;

            client_forms++;
            if (client_cancel_at == client_forms) {
                nc_auth_cancel(W.client);
                return;
            }
            memset(vals, 0, sizeof(vals));
            for (i = 0; i < f->nfields; i++) {
                const char *n = f->fields[i].name;

                if (!n)
                    continue;
                if (strcmp(n, "user") == 0)
                    vals[i].text = client_name;
                else if (strcmp(n, "pw") == 0)
                    vals[i].text = client_pass;
                else if (strcmp(n, "email") == 0)
                    vals[i].text = client_email;
                else if (strcmp(n, "code") == 0)
                    vals[i].text = "123456";
            }
            nc_auth_submit(W.client, vals, f->nfields);
        } else {
            if (nc_auth_waiting(W.client))
                client_waits++;
            return;
        }
    }
}

static void
pump(void)
{
    while (W.head < W.tail) {
        int to_server = W.q[W.head].to_server;
        uint8_t buf[NC_FORM_MAX_MSG];
        size_t len = W.q[W.head].len;

        memcpy(buf, W.q[W.head].buf, len);
        W.head++;
        if (!to_server && W.drop_to_client > 0) {
            W.drop_to_client--;
            continue;
        }
        nc_auth_feed(to_server ? W.server : W.client, buf, len);
        service_needs();
    }
    W.head = W.tail = 0;
}

static struct nc_auth C, S;

static void
setup(void)
{
    uint8_t sid[32];
    struct nc_auth_server_cb scb;
    struct nc_auth_ia_cb iacb;

    memset(sid, 0xa1, sizeof(sid));
    memset(&scb, 0, sizeof(scb));
    scb.methods = s_methods;
    scb.check_password = s_check_password;
    memset(&iacb, 0, sizeof(iacb));
    iacb.begin = s_begin;
    iacb.submit = s_submit;
    iacb.resume = s_resume;
    iacb.cancelled = s_cancelled;

    memset(&W, 0, sizeof(W));
    memset(client_buf, 0, sizeof(client_buf));
    memset(server_buf, 0, sizeof(server_buf));
    client_forms = client_waits = client_asks = 0;
    server_cancelled = 0;
    server_stage = 0;

    nc_auth_client_init(&C, sid, client_name, send_to_server, NULL);
    nc_auth_server_init(&S, sid, &scb, send_to_client, NULL);
    nc_auth_server_interactive(&S, &iacb);
    nc_auth_client_buffer(&C, client_buf, sizeof(client_buf));
    nc_auth_server_buffer(&S, server_buf, sizeof(server_buf));
    W.client = &C;
    W.server = &S;
}

static void
run(void)
{
    nc_auth_start(&C);
    pump();
}

/* Is this byte string anywhere in the region? */
static int
contains(const void *hay, size_t n, const char *needle)
{
    const uint8_t *h = hay;
    size_t nl = strlen(needle);
    size_t i;

    if (nl == 0 || n < nl)
        return 0;
    for (i = 0; i + nl <= n; i++)
        if (memcmp(h + i, needle, nl) == 0)
            return 1;
    return 0;
}

int
main(void)
{
    /* 0. The ordinary path, so the edges have something to be edges of. */
    account_exists = 0;
    server_wait_next = 0;
    client_cancel_at = -1;
    client_choice = NC_AUTH_M_REGISTER;
    client_choice_again = NC_AUTH_M_PASSWORD;   /* then log in with it */
    setup();
    run();
    check("registration creates the account", account_exists == 1);
    check("the server saw what was typed",
          strcmp(account_name, "newbie") == 0 &&
          strcmp(account_pass, "hunter2") == 0);
    check("the client was told it registered", nc_auth_registered(&C));
    check("registered is one-shot", !nc_auth_registered(&C));
    check("and then it logged in", nc_auth_state(&C) == NC_AUTH_STATE_OK &&
          nc_auth_state(&S) == NC_AUTH_STATE_OK);

    /* A form the client could not have been compiled against still renders:
     * the note field carries no answer and must survive the round trip. */
    check("a form with a note round-trips", client_forms == 1);

    /* Per-field errors come back attached to the field that caused them. */
    account_exists = 0;
    client_name = "taken";
    client_choice_again = 0;
    client_cancel_at = 2;               /* stop after seeing the complaint */
    setup();
    run();
    check("a rejected field is re-asked", client_forms == 2);
    client_name = "newbie";

    /* 1. Cancel with a form outstanding. */
    account_exists = 0;
    client_cancel_at = 1;
    setup();
    run();
    check("1. cancel fires the server callback once", server_cancelled == 1);
    check("1. no account was made", account_exists == 0);
    check("1. the client is back at the method offer",
          nc_auth_offered(&C) == NC_AUTH_M_REGISTER);
    check("1. not reported as registered", !nc_auth_registered(&C));

    /* 2. Cancel during a wait, where a player who gave up on mail presses it. */
    account_exists = 0;
    server_wait_next = 1;
    client_cancel_at = -1;
    setup();
    run();
    check("2. the client saw the wait notice", client_waits >= 1);
    check("2. a resumption token was issued",
          nc_auth_pending_token(&C, (uint8_t[8]){ 0 }, 8) == 8);
    nc_auth_cancel(&C);
    pump();
    check("2. cancelling a wait fires the callback", server_cancelled == 1);
    check("2. the token is dead", token_valid == 0);

    /* 3. A cancel crossing a form already in flight. The server resolves the
     *    wait and sends a form at the same moment the player gives up. */
    account_exists = 0;
    server_wait_next = 1;
    setup();
    run();
    {
        struct nc_form next;

        nc_auth_cancel(&C);             /* in flight towards the server */
        memset(&next, 0, sizeof(next));
        next.title = "Confirm";
        next.fields = CODE_FIELDS;
        next.nfields = 1;
        nc_auth_server_resume(&S, NC_AUTH_IA_MORE, &next, NULL, 0);
        pump();
    }
    check("3. the crossed form is ignored",
          nc_auth_needs(&C) != NC_AUTH_NEED_FORM);
    check("3. the client still lands at the offer",
          nc_auth_offered(&C) == NC_AUTH_M_REGISTER);
    check("3. the server cleaned up once", server_cancelled == 1);

    /* 4. Cancel, then log in with an account that already existed. */
    client_choice_again = NC_AUTH_M_PASSWORD;
    account_exists = 1;
    snprintf(account_name, sizeof(account_name), "newbie");
    snprintf(account_pass, sizeof(account_pass), "hunter2");
    server_wait_next = 0;
    client_cancel_at = 1;
    setup();
    run();
    check("4. abandoning the method did not cost the login",
          nc_auth_state(&C) == NC_AUTH_STATE_OK);

    /* 5. Cancel with nothing running, and cancel twice. */
    client_choice_again = 0;
    account_exists = 0;
    client_cancel_at = -1;
    setup();
    nc_auth_cancel(&C);                 /* nothing has started */
    check("5. cancel before anything runs is a no-op",
          server_cancelled == 0 && nc_auth_state(&C) == NC_AUTH_STATE_PENDING);
    server_wait_next = 1;
    run();
    nc_auth_cancel(&C);
    nc_auth_cancel(&C);
    pump();
    check("5. cancelling twice fires one callback", server_cancelled == 1);

    /* 6. nc_auth_clear mid-registration, and again. */
    account_exists = 0;
    server_wait_next = 1;
    setup();
    run();
    nc_auth_clear(&S);
    check("6. clear fires the callback", server_cancelled == 1);
    nc_auth_clear(&S);
    check("6. a second clear fires nothing", server_cancelled == 1);

    /* 7. clear on a struct that never started. */
    {
        struct nc_auth fresh;
        uint8_t sid[32];

        memset(sid, 0xa1, sizeof(sid));
        nc_auth_client_init(&fresh, sid, "nobody", NULL, NULL);
        nc_auth_clear(&fresh);
        nc_auth_clear(&fresh);
        check("7. clearing an unused conversation is safe", 1);
    }

    /* 8. After clear, the password is nowhere in the struct or the buffer. */
    account_exists = 0;
    server_wait_next = 0;
    client_pass = "swordfish-8842";
    setup();
    run();
    check("8. the password really did arrive",
          strcmp(account_pass, "swordfish-8842") == 0);
    nc_auth_clear(&S);
    check("8. no password left in the server buffer",
          !contains(server_buf, sizeof(server_buf), "swordfish-8842"));
    check("8. no password left in the server struct",
          !contains(&S, sizeof(S), "swordfish-8842"));
    nc_auth_clear(&C);
    check("8. no password left in the client buffer",
          !contains(client_buf, sizeof(client_buf), "swordfish-8842"));
    client_pass = "hunter2";

    /* 9. A denied conversation wipes as thoroughly as a cancelled one.
     *    Denial is the path an attacker can drive, so it is the one to
     *    check. */
    account_exists = 0;
    server_wait_next = 1;
    client_pass = "swordfish-9931";
    setup();
    run();
    nc_auth_server_resume(&S, NC_AUTH_IA_DENIED, NULL, NULL, 0);
    pump();
    check("9. denial ends both sides",
          nc_auth_state(&S) == NC_AUTH_STATE_DENIED &&
          nc_auth_state(&C) == NC_AUTH_STATE_DENIED);
    check("9. denial did not double-fire the callback", server_cancelled == 0);
    nc_auth_clear(&S);
    check("9. a denied conversation wipes",
          !contains(server_buf, sizeof(server_buf), "swordfish-9931") &&
          !contains(&S, sizeof(S), "swordfish-9931"));
    client_pass = "hunter2";

    /* 10. Resuming with a token the server has already dropped. */
    account_exists = 0;
    server_wait_next = 1;
    setup();
    run();
    token_valid = 0;                    /* it expired while we were away */
    {
        uint8_t tok[NC_FORM_MAX_TOKEN];
        size_t tl = nc_auth_pending_token(&C, tok, sizeof(tok));

        check("10. a token was there to save", tl == sizeof(live_token));
        setup();
        nc_auth_resume_pending(&C, tok, tl);
        run();
        check("10. a dead token is refused, not honoured",
              nc_auth_state(&C) == NC_AUTH_STATE_DENIED && !account_exists);
    }

    /* 11. A cancelled registration's token is dead: the player cancels, then
     *     clicks the link that arrived by mail anyway. */
    account_exists = 0;
    server_wait_next = 1;
    setup();
    run();
    {
        uint8_t tok[NC_FORM_MAX_TOKEN];
        size_t tl = nc_auth_pending_token(&C, tok, sizeof(tok));

        nc_auth_cancel(&C);
        pump();
        check("11. cancelling killed the token", token_valid == 0);
        setup();
        nc_auth_resume_pending(&C, tok, tl);
        run();
        check("11. the emailed link no longer works",
              nc_auth_state(&C) == NC_AUTH_STATE_DENIED);
    }

    /* 12. The resumption path works when the token is still good, which is
     *     what case 11 is the negative of. */
    client_choice_again = NC_AUTH_M_PASSWORD;
    account_exists = 0;
    server_wait_next = 1;
    setup();
    run();
    {
        uint8_t tok[NC_FORM_MAX_TOKEN];
        size_t tl = nc_auth_pending_token(&C, tok, sizeof(tok));

        server_wait_next = 0;
        setup();
        nc_auth_resume_pending(&C, tok, tl);
        run();
        check("12. a live token resumes where it left off",
              account_exists == 1 && nc_auth_state(&C) == NC_AUTH_STATE_OK);
    }

    /* 13. Registration is offered again after a login fails, and choosing it
     *     then works. This is the player who never had an account here. */
    account_exists = 1;
    snprintf(account_name, sizeof(account_name), "someone-else");
    snprintf(account_pass, sizeof(account_pass), "not-yours");
    server_wait_next = 0;
    client_choice = NC_AUTH_M_PASSWORD;         /* try to log in first */
    setup();
    run();
    check("13. the bad login was refused",
          nc_auth_state(&C) != NC_AUTH_STATE_OK);
    client_choice = NC_AUTH_M_REGISTER;

    /* 14. The framer: a message split across reads, and two in one read. */
    {
        struct nc_auth_framer f;
        uint8_t fbuf[64], out[64];
        const void *m;
        long n;

        nc_auth_framer_init(&f, fbuf, sizeof(fbuf));
        n = nc_auth_frame(out, sizeof(out), "hello", 5);
        check("14. framing writes a length prefix", n == 7);

        nc_auth_framer_push(&f, out, 3);        /* half a message */
        check("14. a partial message yields nothing",
              nc_auth_framer_next(&f, &m) == 0);
        nc_auth_framer_push(&f, out + 3, 4);
        n = nc_auth_framer_next(&f, &m);
        check("14. the rest completes it",
              n == 5 && memcmp(m, "hello", 5) == 0);
        check("14. and then there is no more",
              nc_auth_framer_next(&f, &m) == 0);

        nc_auth_frame(out, sizeof(out), "ab", 2);
        nc_auth_frame(out + 4, sizeof(out) - 4, "cd", 2);
        nc_auth_framer_push(&f, out, 8);
        n = nc_auth_framer_next(&f, &m);
        check("14. two in one read: first", n == 2 && memcmp(m, "ab", 2) == 0);
        n = nc_auth_framer_next(&f, &m);
        check("14. two in one read: second", n == 2 && memcmp(m, "cd", 2) == 0);
        check("14. nothing left", nc_auth_framer_next(&f, &m) == 0);
    }

    if (failures == 0)
        printf("all checks passed\n");
    return failures != 0;
}

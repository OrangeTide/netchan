/* nc_auth.h : SSH-shaped client authentication over an established session */

#ifndef NC_AUTH_H
#define NC_AUTH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * nc_crypto authenticates the *server* to the client, because the client
 * knows the server's identity key in advance. It says nothing about who the
 * client is. That question belongs to the application, and this file is one
 * answer to it: a small conversation modelled on ssh's userauth, carried as
 * ordinary reliable messages over the encrypted channel.
 *
 * The flow is ssh's, minus the parts a game does not need:
 *
 *   client -> HELLO    "I claim to be <user>"
 *   server -> METHODS  "for that name I will accept publickey, password"
 *   client -> PUBKEY   a public key and a signature, tried first
 *   server -> OK, or FAIL carrying the methods still worth trying
 *   client -> PASSWORD only if publickey was refused or unavailable
 *   server -> OK or FAIL
 *
 * Public-key authentication is what makes a login instant: the client's
 * secret key sits on disk, encrypted under a local passphrase, and unlocking
 * it costs nothing on the network. Password entry is the fallback for a
 * client with no key enrolled yet, exactly as with ssh.
 *
 * A third method, the interactive one at the bottom of this file, carries
 * everything that is neither of those: registering an account, confirming an
 * emailed code, a one-time token, a second factor. It is one method rather
 * than one per scheme, and nothing in here learns what any of them mean.
 *
 * THE SIGNATURE MUST BE BOUND TO THE SESSION
 *
 * The signed message is a digest over a fixed label, the nc_crypto session
 * id, the username, and the public key. Binding to the session id is not
 * decoration, it is the whole security of the scheme. A signature over a
 * bare challenge could be collected by a malicious server and replayed to a
 * real one to log in as the client. The session id commits to both ephemeral
 * keys and both identity keys, so a signature produced for one session is
 * worthless everywhere else. ssh does the same thing with its session
 * identifier, and for the same reason.
 *
 * This file has no idea what a socket or a netchan channel is. It is handed
 * a send callback and fed whole messages, so it can be driven by a test with
 * two state machines in one process and no transport at all.
 *
 * ASKING FOR A CREDENTIAL WITHOUT BLOCKING
 *
 * A client cannot answer "what is your password" from inside a function call,
 * because the answer comes from a human. A callback that returns the password
 * would have to sit and wait for one, and a program driven by an event loop
 * cannot afford to sit anywhere: its timers stop, and on this protocol that
 * means the connection times out while the user is still typing.
 *
 * So the conversation suspends instead. When it needs a credential it sets
 * nc_auth_needs() and returns, sending nothing. The application collects the
 * answer however it likes, taking as long as it likes, and calls
 * nc_auth_supply_key() or nc_auth_supply_password() to resume. Passing NULL
 * means "I have nothing", and the conversation moves on to the next method.
 *
 * This costs the client side its callbacks entirely, which is a fair trade:
 * the state machine no longer calls out into code that might block, so there
 * is nowhere left for a blocking read to hide.
 */

/* Method bits, as carried in METHODS and FAIL. */
#define NC_AUTH_M_PUBKEY       0x01
#define NC_AUTH_M_PASSWORD     0x02
#define NC_AUTH_M_INTERACTIVE  0x04  /* log in by answering questions */
#define NC_AUTH_M_REGISTER     0x08  /* create an account by answering them */

#define NC_AUTH_MAX_USER    64
#define NC_AUTH_MAX_PASS    128
#define NC_AUTH_MAX_MSG     256
#define NC_AUTH_MAX_TRIES   6

#define NC_AUTH_OK  (0)
#define NC_AUTH_ERR (-1)

/****************************************************************
 * Bounds on an interactive exchange
 *
 * Every one of these limits what an unauthenticated stranger can make the far
 * side hold. They are not sized to be generous.
 ****************************************************************/

#define NC_FORM_MAX_FIELDS    24
#define NC_FORM_MAX_OPTIONS   16
#define NC_FORM_MAX_NAME      32   /* the machine name of a field */
#define NC_FORM_MAX_LABEL    128   /* what the player reads beside the box */
#define NC_FORM_MAX_VALUE    256   /* one submitted answer */
#define NC_FORM_MAX_ERRORS   500   /* all error text in one form, in bytes */
#define NC_FORM_MAX_TOKEN     64   /* a resumption or bearer token */
/*
 * One encoded form, or one set of answers.
 *
 * Sized to fit the carrier rather than chosen for roundness. netchan's own
 * reliable message cap is 2048 bytes, and a carrier wants a few for itself: a
 * tag byte here, a length prefix there. A form that cannot cross the transport
 * this library ships is a form nobody can use, so the ceiling sits below that
 * cap rather than above it.
 */
#define NC_FORM_MAX_MSG     2000

/**
 * Conversation state, as reported by nc_auth_state. These are states, not
 * return codes: NC_AUTH_STATE_OK is 1 and says the client is authenticated,
 * while NC_AUTH_OK is 0 and says a call succeeded.
 */
enum {
    NC_AUTH_STATE_PENDING,        /* conversation still in progress */
    NC_AUTH_STATE_OK,             /* the client is authenticated */
    NC_AUTH_STATE_DENIED,         /* no method left, or too many attempts */
};

/** What the client conversation is waiting for. See nc_auth_needs. */
enum {
    NC_AUTH_NEED_NOTHING = 0,
    NC_AUTH_NEED_KEY,       /* an Ed25519 key pair, or NULL to skip */
    NC_AUTH_NEED_PASSWORD,  /* a password, or NULL to skip */
    NC_AUTH_NEED_METHOD,    /* log in, or make an account? */
    NC_AUTH_NEED_FORM,      /* here are the questions; answer them */
};

struct nc_auth;

/** Hand a complete auth message to the transport. */
typedef void (*nc_auth_send_cb)(void *ctx, const void *msg, size_t len);

struct nc_auth_server_cb {
    /*
     * Which methods to offer for this name, as a bitmask. Returning 0
     * refuses the name outright.
     *
     * Offering the same methods for an unknown user as for a known one is
     * deliberate in the demo's store: answering differently turns the
     * handshake into a way to enumerate accounts. Registration undoes that
     * on its own, since a signup form has to say when a name is taken, and
     * that leak is accepted rather than defended against.
     */
    unsigned (*methods)(void *ctx, const char *user);
    /* True if pk is an authorised key for user. The signature over it
     * has already been checked by the time this is called. */
    bool (*check_key)(void *ctx, const char *user, const uint8_t pk[32]);
    /* True if the password is correct for user. */
    bool (*check_password)(void *ctx, const char *user, const char *password);
    void *ctx;
};

/****************************************************************
 * Forms
 ****************************************************************/

/*
 * The types are the ones a registration needs and no more. There is no hidden
 * field, because continuation state is keyed by something the server already
 * holds, the session or the resumption token, rather than echoed through a
 * client that may edit it. There are no buttons, because a game draws its own
 * interface and already knows a form has one submit action. There is no file
 * or image field, because nothing here wants transfer machinery.
 */
enum nc_form_type {
    NC_FF_TEXT = 1,   /* a line of UTF-8 */
    NC_FF_PASSWORD,   /* a line the client must not echo or log */
    NC_FF_EMAIL,      /* user@host */
    NC_FF_INTEGER,    /* a whole number, optionally ranged */
    NC_FF_PHONE,      /* digits the client may format by region */
    NC_FF_BOOL,       /* one yes or no */
    NC_FF_CHOICE,     /* one of a list, or several of it */
    NC_FF_NOTE,       /* no answer: text for the player to read */
    NC_FF_LINK,       /* no answer: a URL to open outside the game */
};

#define NC_FF_REQUIRED  0x01  /* the client should refuse to submit it empty */
#define NC_FF_MULTI     0x02  /* NC_FF_CHOICE: more than one may be picked */

/**
 * One field of a form.
 *
 * Nothing here is written to and every string is borrowed, so the same struct
 * serves a form compiled in as a static table and a form an operator wrote in
 * a config file and the server parsed at startup. The second case is the
 * common one and the reason the format describes itself: two shards of one
 * game are not configured alike, one asks for a date of birth and the next
 * does not, and a client that has to be rebuilt to see a new field is a client
 * that cannot follow its own game.
 *
 * size, maxlength, min, max, pattern, and NC_FF_REQUIRED are rendering and
 * typo-catching hints only. The server revalidates every answer on arrival and
 * applies its own limits whatever the form said. A client that ignores them
 * all is rude, not dangerous.
 *
 * A client needs no layout engine to render one of these. Top to bottom, one
 * field per row, in the order the form lists them, is enough and is what the
 * format is shaped for.
 */
struct nc_form_field {
    uint8_t     type;        /* enum nc_form_type */
    uint8_t     flags;       /* NC_FF_* */
    const char *name;        /* machine name, unique within the form */
    const char *label;       /* what the player reads */
    const char *value;       /* default; the text of a NOTE; the URL of a LINK */
    const char *pattern;     /* TEXT, PASSWORD: what a valid answer looks like */
    const char *error;       /* set on a re-issued form, else NULL */
    uint16_t    size;        /* suggested width of the box, in characters */
    uint16_t    maxlength;   /* suggested cap on the answer */
    int32_t     min, max;    /* INTEGER: inclusive range, equal to disable */
    const char *const *options;  /* CHOICE */
    uint8_t     noptions;
};

/** A message attached to the field that caused it, sent with a re-issued
 * form. The encoder folds these into the records they name, and a decoded
 * form carries them back in nc_form_field.error. */
struct nc_form_error {
    const char *name;        /* the field it belongs beside */
    const char *message;
};

/**
 * A form as the server hands it over, and as the client reads it back.
 *
 * title and note are the form's own text. The errors array is what turns a
 * rejection into something a player can act on: without it, six boxes and one
 * complaint means guessing which box was wrong. A server need not explain every
 * failure, and a form with no errors at all still says the submission was
 * refused. Keep the combined message text inside NC_FORM_MAX_ERRORS. Anything
 * longer belongs in a NOTE, where it is read once instead of on every retry.
 */
struct nc_form {
    const char *title;
    const char *note;
    const struct nc_form_field *fields;
    uint8_t     nfields;
    const struct nc_form_error *errors;
    uint8_t     nerrors;
};

/**
 * One answer. Which member is meaningful follows the field's type: text for
 * TEXT, PASSWORD, EMAIL, and PHONE; number for INTEGER; on for BOOL; choice
 * for CHOICE, as an index, or as a bitmask when the field is NC_FF_MULTI.
 * NOTE and LINK take no answer and are skipped.
 */
struct nc_form_value {
    const char *text;
    int32_t     number;
    uint32_t    choice;
    bool        on;
};

/* What a server callback decides about a submission. */
enum {
    NC_AUTH_IA_MORE = 0,  /* another form follows; fill *out */
    NC_AUTH_IA_DONE,      /* the account exists, or the login succeeded */
    NC_AUTH_IA_DENIED,    /* refuse, and end the conversation */
    NC_AUTH_IA_WAIT,      /* the slow thing has been started; answer later */
};

struct nc_auth_ia_cb {
    /**
     * The opening form for the method the client selected. Return
     * NC_AUTH_IA_MORE having filled *out, or NC_AUTH_IA_DENIED to refuse.
     */
    int (*begin)(void *ctx, unsigned method, struct nc_form *out);

    /**
     * A submission arrived. Read the answers with nc_auth_value_*, then
     * decide. Filling *out on NC_AUTH_IA_MORE re-asks, with errors attached
     * to whichever fields were wrong.
     *
     * The server may not block here either, and for a duller reason than the
     * client: sending mail, reaching an SMS gateway, or asking an identity
     * provider is slow, and one thread is serving every other connection.
     * Return NC_AUTH_IA_WAIT to start the slow thing and answer later, and
     * fill out->note with what to tell the player meanwhile.
     */
    int (*submit)(void *ctx, struct nc_auth *a, struct nc_form *out);

    /**
     * A client presented a resumption token. Return NC_AUTH_IA_MORE with the
     * form it left off at, or NC_AUTH_IA_DENIED if the token has expired or
     * was never issued. Optional: without it, tokens are refused.
     */
    int (*resume)(void *ctx, const uint8_t *tok, size_t len,
                  struct nc_form *out);

    /**
     * An interactive method that was running has ended without finishing,
     * because the client cancelled or because the application called
     * nc_auth_clear. Drop whatever was pending and invalidate its token.
     *
     * Invalidating is the part that matters. Mail already sent cannot be
     * recalled, so a player who cancels and then clicks the link that arrived
     * anyway must find it dead.
     *
     * Fires at most once per conversation, and not at all if nothing was
     * running. Optional.
     */
    void (*cancelled)(void *ctx);

    void *ctx;
};

/****************************************************************
 * The conversation
 ****************************************************************/

struct nc_auth {
    int      server;                    /* which side of the conversation */
    int      state;
    int      need;                      /* client: NC_AUTH_NEED_* */
    int      greeted;                   /* server: HELLO seen */
    unsigned avail;                     /* methods still worth trying */
    int      tries;
    uint8_t  sid[32];                   /* nc_crypto session id */
    char     user[NC_AUTH_MAX_USER + 1];
    nc_auth_send_cb send;
    void    *send_ctx;
    struct nc_auth_server_cb scb;

    /* the interactive method */
    struct nc_auth_ia_cb iacb;
    uint8_t *buf;                       /* caller's form and answer buffer */
    size_t   buflen;
    size_t   strings;                   /* bytes of decoded strings in buf */
    unsigned offered;                   /* client: the mask METHODS carried */
    unsigned ia_method;                 /* the bit now running, 0 if none */
    unsigned chosen;                    /* client: answer to NEED_METHOD */
    uint8_t  chose;                     /* client: chosen holds an answer */
    uint8_t  asked;                     /* client: NEED_METHOD already put */
    uint8_t  cancelling;                /* client: ignore until METHODS */
    uint8_t  registered;                /* client: one-shot, see below */
    uint8_t  fired;                     /* server: cancelled callback spent */
    struct nc_form form;                /* client: the decoded form */
    const char *wait_note;              /* client: what it is waiting on */
    uint8_t  token[NC_FORM_MAX_TOKEN];
    uint8_t  token_len;
};

void nc_auth_client_init(struct nc_auth *a, const uint8_t sid[32],
                         const char *user,
                         nc_auth_send_cb send, void *send_ctx);

void nc_auth_server_init(struct nc_auth *a, const uint8_t sid[32],
                         const struct nc_auth_server_cb *cb,
                         nc_auth_send_cb send, void *send_ctx);

/** Client only: emit the opening HELLO. Harmless on the server. */
void nc_auth_start(struct nc_auth *a);

/** Feed one complete message. Returns NC_AUTH_OK, or NC_AUTH_ERR if the peer
 * malformed or out of order, which also ends the conversation as denied. */
int nc_auth_feed(struct nc_auth *a, const void *msg, size_t len);

/**
 * What the conversation is waiting for, or NC_AUTH_NEED_NOTHING. Check this
 * after every nc_auth_feed. While it is non-zero the client has sent nothing
 * and is waiting; nothing times out inside nc_auth, so the application may
 * take as long as it needs.
 */
int nc_auth_needs(const struct nc_auth *a);

/**
 * Resume a suspended conversation. Pass NULL to say the credential is not
 * available, which drops that method and moves on to the next one. Calling
 * either function when nc_auth_needs() does not match is a no-op.
 *
 * nc_auth does not retain the secret: the key is used to sign immediately and
 * the password is copied into the outgoing message and wiped.
 */
void nc_auth_supply_key(struct nc_auth *a, const uint8_t sk[64],
                        const uint8_t pk[32]);
void nc_auth_supply_password(struct nc_auth *a, const char *password);

int nc_auth_state(const struct nc_auth *a);

/** The authenticated name. Only meaningful once the state is
 * NC_AUTH_STATE_OK. */
const char *nc_auth_user(const struct nc_auth *a);

/**
 * The digest a client signs and a server checks:
 *   BLAKE2b-256("netchan-auth-v1" || sid || ulen || user || pk)
 * Exposed so a key-management tool can produce the same bytes.
 */
void nc_auth_signed_digest(uint8_t out[32], const uint8_t sid[32],
                           const char *user, const uint8_t pk[32]);

/**
 * End the conversation and wipe it. Wipes the struct and the supplied buffer
 * rather than resetting their lengths, fires the cancelled callback if an
 * interactive method was running, and leaves a struct that is safe to discard.
 *
 * Call it when the session goes away, however it goes away. Safe on a struct
 * that never started, safe twice, and the callback still fires only once.
 *
 * What it cannot reach is the transport's own copy of the last message sent.
 * A password that went out through the send callback is in whatever buffer the
 * caller handed the socket, and clearing that is the caller's to do.
 */
void nc_auth_clear(struct nc_auth *a);

/****************************************************************
 * The interactive method, client side
 *
 * CHOOSING A METHOD IS A QUESTION FOR THE HUMAN
 *
 * The client machine picks its own order today: publickey, then password if
 * that is refused. It can, because both are attempts to log in as the same
 * person and failing from one to the next costs nothing and asks nobody
 * anything.
 *
 * Registration is not a link in that chain. It is a different intent, and no
 * ordering rule can infer it: the player pressed "create account" rather than
 * "log in", and only the player knows that. So it arrives the way every other
 * human answer arrives here, by suspending and being supplied.
 *
 * The conversation only suspends when there is a choice worth making, which in
 * practice means when the server offered NC_AUTH_M_REGISTER. Offered publickey
 * and password alone, the machine chains as it always did and the application
 * sees no change.
 *
 * WAITING IS A STATE, NOT A GAP
 *
 * Between "I gave you my address" and "here is the code from the mail" the
 * client has nothing to do and nothing to show, and a client showing nothing
 * looks broken. So the server says so out loud, and both ends move into a
 * state that is explicitly about waiting for it.
 *
 * The client is not suspended there. nc_auth_needs() is NC_AUTH_NEED_NOTHING,
 * because there is no question outstanding. It is waiting for the server to
 * speak again, which it does with the next form, or with OK, or with FAIL.
 ****************************************************************/

/**
 * Answer NC_AUTH_NEED_METHOD with one NC_AUTH_M_* bit. Passing 0 means none of
 * them are wanted, which ends the conversation, and is what a cancelled login
 * screen calls. Passing a bit that is not interactive, or several, means
 * "log in normally" and hands control back to the publickey and password
 * chain.
 *
 * May be called before METHODS arrives, and usually is: a player who clicked
 * "create account" in the main menu decided before the socket was open. The
 * choice is remembered and applied when the offer lands, so the conversation
 * never suspends and the login screen never flickers.
 */
void nc_auth_supply_method(struct nc_auth *a, unsigned method);

/** What the server offered, once METHODS has arrived: a mask of NC_AUTH_M_*
 * bits, or 0 before that. A client draws its "create account" button from
 * this, which is the whole reason registration is a bit in METHODS rather than
 * a question inside the first form. */
unsigned nc_auth_offered(const struct nc_auth *a);

/**
 * The form the client is suspended on. The returned pointers are into the
 * buffer the caller supplied, so they stay valid until the next nc_auth_feed.
 * Returns NULL when nc_auth_needs() is not NC_AUTH_NEED_FORM.
 */
const struct nc_form *nc_auth_form(const struct nc_auth *a);

/**
 * Answer the form and resume. vals is parallel to the form's fields, so the
 * client indexes rather than matching strings; the names travel on the wire so
 * the server never has to trust the order it gets back. Entries for NOTE and
 * LINK fields are ignored.
 *
 * As with nc_auth_supply_password, nothing is retained: a PASSWORD answer is
 * copied into the outgoing message and the copy is wiped.
 */
void nc_auth_submit(struct nc_auth *a, const struct nc_form_value *vals, int n);

/**
 * Abandon the interactive method. Works with a form on screen and works during
 * a wait, which is where a player who has given up on an email actually
 * presses it.
 *
 * This abandons the method, not the login. The conversation returns to the
 * point where the server offered methods, exactly as a completed registration
 * does, so a player who changed their mind can log in with the account they
 * already had without reconnecting. To abandon the login itself, answer
 * NC_AUTH_NEED_METHOD with 0.
 *
 * A no-op when no interactive method is running, and harmless twice.
 *
 * Nothing here is load bearing. A client that crashes sends no cancel, so a
 * server must reach the same end state without one. Cancelling only makes it
 * immediate, which matters because the slot it frees is capped per address:
 * the player who changed their mind stops holding one, and a griefer opening
 * registrations to abandon them is left paying the expiry on every slot.
 */
void nc_auth_cancel(struct nc_auth *a);

/**
 * What the client is waiting on an external service to finish, as the server
 * described it: "we sent a code to j@example.com". NULL when it is not
 * waiting. The client shows this and stops drawing a form.
 */
const char *nc_auth_waiting(const struct nc_auth *a);

/**
 * True once, after a registration completes. The conversation then returns to
 * the state before it, the server offers methods again, and the client logs in
 * normally with what it just enrolled. This flag exists only so the client can
 * say so to the player.
 */
bool nc_auth_registered(struct nc_auth *a);

/****************************************************************
 * Resuming later
 *
 * A registration waiting on an emailed code stays open for minutes. The
 * connection survives that on netchan's keepalive. The player does not: they
 * read the mail on a phone, or the game crashes, or the wifi drops. So the
 * pending registration lives on the server under a token, and a fresh
 * connection can present that token and carry on.
 ****************************************************************/

/** Copy out the resumption token, if the server issued one. Returns its
 * length, or 0 if there is none to save. */
size_t nc_auth_pending_token(const struct nc_auth *a, uint8_t *out, size_t len);

/** Offer a saved token instead of starting the form over. Call before
 * answering NC_AUTH_NEED_METHOD. */
void nc_auth_resume_pending(struct nc_auth *a, const uint8_t *tok, size_t len);

/****************************************************************
 * The interactive method, server side
 ****************************************************************/

/**
 * Register the interactive callbacks. The methods callback in
 * nc_auth_server_cb decides which of the two bits to offer, and is asked again
 * after a registration completes, because by then the answer has changed.
 */
void nc_auth_server_interactive(struct nc_auth *a,
                                const struct nc_auth_ia_cb *cb);

/**
 * Read an answer by name, from inside a submit callback. The typed forms
 * return the fallback when the field is absent or holds another type.
 *
 * The pointer nc_auth_value_text returns is into the server's answer buffer
 * and is wiped when the conversation moves on, so a password must be checked
 * or hashed here rather than saved for later.
 */
const char *nc_auth_value_text(const struct nc_auth *a, const char *name);
int32_t     nc_auth_value_int(const struct nc_auth *a, const char *name,
                              int32_t fallback);
bool        nc_auth_value_bool(const struct nc_auth *a, const char *name,
                               bool fallback);
uint32_t    nc_auth_value_choice(const struct nc_auth *a, const char *name);

/**
 * Issue the resumption token for what is now pending, from inside a callback
 * and before returning NC_AUTH_IA_WAIT.
 *
 * The wait is exactly when the client needs it. A player who reads the mail on
 * their phone, or whose game falls over, comes back on a new connection with
 * nothing but this, so handing it over only once the wait resolves would be
 * handing it over after the moment it was for.
 */
void nc_auth_server_token(struct nc_auth *a, const uint8_t *tok, size_t len);

/**
 * Finish a submission that returned NC_AUTH_IA_WAIT: the mail went out, the
 * gateway answered, the player clicked the link. verdict is one of
 * NC_AUTH_IA_MORE, _DONE, or _DENIED, and form is read only when it is _MORE,
 * which is the usual answer because the next thing to do is ask for the code
 * that was just sent.
 *
 * Issuing a token here is what lets the client come back to this registration
 * on another connection.
 */
void nc_auth_server_resume(struct nc_auth *a, int verdict,
                           const struct nc_form *form,
                           const uint8_t *token, size_t token_len);

/****************************************************************
 * Buffers
 *
 * A form arrives in a message the caller owns and may reuse the moment
 * nc_auth_feed returns, and the conversation then suspends for a human. So the
 * form has to be copied somewhere, and nc_auth neither allocates nor carries a
 * buffer large enough to hold one. The caller supplies it, as the caller
 * supplies everything else here.
 *
 * A NULL buffer means the interactive method is unavailable on that side,
 * which is the right default for a peer that only ever does publickey. It is
 * also the real cap on what an unauthenticated stranger can make this end
 * hold.
 ****************************************************************/

void nc_auth_client_buffer(struct nc_auth *a, void *buf, size_t len);
void nc_auth_server_buffer(struct nc_auth *a, void *buf, size_t len);

/****************************************************************
 * Framing
 *
 * nc_auth is fed whole messages and knows nothing of what carries them, which
 * is what lets a test drive two state machines in one process with no
 * transport at all. That does not change. But a form no longer fits in a
 * datagram, so auth moves to a reliable stream channel, and a stream hands
 * back bytes rather than messages. Somebody has to write a length in front of
 * each one and reassemble on the far side.
 *
 * Every caller would write the same twenty lines, and half of them would get
 * the partial-read case wrong, so it lives here. It still knows nothing about
 * netchan: bytes go in, messages come out.
 ****************************************************************/

/** Write msg into out with its 2-byte little-endian length in front. Returns
 * the bytes written, or NC_AUTH_ERR if out is too small. */
long nc_auth_frame(void *out, size_t outlen, const void *msg, size_t len);

/** Reassembles messages from a byte stream, into a buffer the caller owns. */
struct nc_auth_framer {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    size_t   done;   /* handed out already, dropped on the next call */
};

void nc_auth_framer_init(struct nc_auth_framer *f, void *buf, size_t cap);

/** Add bytes read from the channel. Returns NC_AUTH_OK, or NC_AUTH_ERR when
 * the peer has sent more than the buffer holds, which ends the conversation:
 * a message that does not fit was never going to be answered. */
int nc_auth_framer_push(struct nc_auth_framer *f, const void *bytes, size_t n);

/**
 * Take the next whole message, if one has arrived. Returns its length and
 * points *msg at it, or 0 when more bytes are needed. The message stays valid
 * until the next push or next call, which is long enough to hand it straight
 * to nc_auth_feed.
 *
 *     nc_auth_framer_push(&f, rx, n);
 *     while ((mlen = nc_auth_framer_next(&f, &m)) > 0)
 *         nc_auth_feed(&a, m, mlen);
 */
long nc_auth_framer_next(struct nc_auth_framer *f, const void **msg);

#endif /* NC_AUTH_H */

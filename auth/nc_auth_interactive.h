/* nc_auth_interactive.h : API sketch for the interactive auth method */

/*
 * THIS FILE IS A SKETCH. Nothing implements it, and it is not in module.mk.
 * It exists to be argued with. See docs/content/registration/index.md for the
 * design it follows.
 *
 * These declarations belong in nc_auth.h once they settle. They are kept
 * apart only so the real header keeps describing code that exists.
 *
 * WHAT THIS ADDS
 *
 * nc_auth today knows two methods, publickey and password, each with its own
 * message and its own state. A third scheme would need a third of each, and a
 * fourth a fourth. The interactive method ends that: the server sends a form,
 * the client renders it, the answers come back, and the loop repeats until the
 * server is satisfied. Registration, email confirmation, a one-time token, and
 * a second factor are all the same three messages.
 *
 * The method carries no scheme of its own and never learns what an account is.
 */

#ifndef NC_AUTH_INTERACTIVE_H
#define NC_AUTH_INTERACTIVE_H

#include "nc_auth.h"

/****************************************************************
 * Method bits, added to those in nc_auth.h
 ****************************************************************/

/*
 * Two bits, one loop. They differ in what the client is asking for, which is
 * the only thing the client cannot work out for itself: whether this server
 * will create an account at all, and so whether to draw the button.
 */
#define NC_AUTH_M_INTERACTIVE  0x04  /* log in by answering questions */
#define NC_AUTH_M_REGISTER     0x08  /* create an account by answering them */

/****************************************************************
 * Bounds
 *
 * Every one of these is a limit on what an unauthenticated stranger can make
 * the far side hold. They are not sized to be generous.
 ****************************************************************/

#define NC_FORM_MAX_FIELDS    24
#define NC_FORM_MAX_OPTIONS   16
#define NC_FORM_MAX_NAME      32   /* the machine name of a field */
#define NC_FORM_MAX_LABEL    128   /* what the player reads beside the box */
#define NC_FORM_MAX_VALUE    256   /* one submitted answer */
#define NC_FORM_MAX_ERRORS   500   /* all error text in one form, in bytes */
#define NC_FORM_MAX_TOKEN     64   /* a resumption or bearer token */

/****************************************************************
 * Fields
 ****************************************************************/

/*
 * The types are the ones a registration needs and no more. There is no hidden
 * field, because continuation state is keyed by something the server already
 * holds, the session or the resumption token, rather than echoed through a
 * client that may edit it. There are no buttons, because a game draws its own
 * interface and already knows a form has one submit action. There is no file or
 * image field, because nothing here wants transfer machinery.
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
 */
struct nc_form_field {
    uint8_t     type;        /* enum nc_form_type */
    uint8_t     flags;       /* NC_FF_* */
    const char *name;        /* machine name, unique within the form */
    const char *label;       /* what the player reads */
    const char *value;       /* default; the text of a NOTE; the URL of a LINK */
    const char *pattern;     /* TEXT, PASSWORD: what a valid answer looks like */
    uint16_t    size;        /* suggested width of the box, in characters */
    uint16_t    maxlength;   /* suggested cap on the answer */
    int32_t     min, max;    /* INTEGER: inclusive range, equal to disable */
    const char *const *options;  /* CHOICE */
    uint8_t     noptions;
};

/** A message attached to the field that caused it, sent with a re-issued form. */
struct nc_form_error {
    const char *name;        /* the field it belongs beside */
    const char *message;
};

/**
 * A form as the server hands it over.
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

/****************************************************************
 * Answers
 ****************************************************************/

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

/****************************************************************
 * The client side
 *
 * The client keeps no callbacks, for the reason nc_auth.h already gives: the
 * answer comes from a human, a human is slow, and a state machine that calls
 * out into code which might wait is a state machine with a place for a blocking
 * read to hide. So the conversation suspends. nc_auth_needs() reports
 * NC_AUTH_NEED_FORM, the application renders and collects for as long as it
 * likes, and nc_auth_submit() resumes it.
 ****************************************************************/

/* Both join the NC_AUTH_NEED_* enum in nc_auth.h. */
#define NC_AUTH_NEED_METHOD  3   /* log in, or make an account? */
#define NC_AUTH_NEED_FORM    4   /* here are the questions; answer them */

/*
 * WAITING IS A STATE, NOT A GAP
 *
 * Between "I gave you my address" and "here is the code from the mail" the
 * client has nothing to do and nothing to show, and a client showing nothing
 * looks broken. So the server says so out loud: it sends a wait message
 * meaning the slow thing has been started, and both ends move into a state
 * that is explicitly about waiting for it.
 *
 * The client is not suspended here. nc_auth_needs() is NC_AUTH_NEED_NOTHING,
 * because there is no question outstanding. It is waiting for the server to
 * speak again, which it does with the next form, or with OK, or with FAIL.
 */

/*
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
 */

/**
 * Answer NC_AUTH_NEED_METHOD with one NC_AUTH_M_* bit. Passing 0 means none of
 * them are wanted, which ends the conversation, and is what a cancelled login
 * screen calls.
 *
 * May be called before METHODS arrives, and usually is: a player who clicked
 * "create account" in the main menu decided before the socket was open. The
 * choice is remembered and applied when the offer lands, so the conversation
 * never suspends and the login screen never flickers. If the server turns out
 * not to offer the chosen method, the conversation suspends and asks properly.
 */
void nc_auth_supply_method(struct nc_auth *a, unsigned method);

/** What the server offered, once METHODS has arrived: a mask of NC_AUTH_M_*
 * bits, or 0 before that. A client draws its "create account" button from
 * this, which is the whole reason registration is a bit in METHODS rather than
 * a question inside the first form. */
unsigned nc_auth_offered(const struct nc_auth *a);

/**
 * The form the client is suspended on. The returned pointers are into the
 * client's own form buffer, so they stay valid until the next nc_auth_feed.
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
 * nc_auth_select. */
void nc_auth_resume_pending(struct nc_auth *a, const uint8_t *tok, size_t len);

/****************************************************************
 * The server side
 ****************************************************************/

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

/**
 * Register the interactive callbacks. The methods callback in
 * nc_auth_server_cb decides which of the two bits to offer, and is asked again
 * after a registration completes, because by then the answer has changed.
 */
void nc_auth_server_interactive(struct nc_auth *a,
                                const struct nc_auth_ia_cb *cb);

/****************************************************************
 * Buffers
 *
 * A form arrives in a message the caller owns and may reuse the moment
 * nc_auth_feed returns, and the conversation then suspends for a human. So the
 * form has to be copied somewhere, and nc_auth neither allocates nor carries a
 * buffer large enough to hold one. The caller supplies it, as the caller
 * supplies everything else here.
 *
 * These extend the existing init calls rather than standing alone. A NULL
 * buffer means the interactive method is unavailable on that side, which is
 * the right default for a peer that only ever does publickey.
 ****************************************************************/

void nc_auth_client_buffer(struct nc_auth *a, void *buf, size_t len);
void nc_auth_server_buffer(struct nc_auth *a, void *buf, size_t len);

/****************************************************************
 * Ending, and cleaning up
 *
 * An interactive conversation ends without finishing in three ways, and all
 * three have to converge on the same cleanup or a half-made account outlives
 * the attempt that made it.
 *
 *   cancel   the client said so. Immediate, and never guaranteed.
 *   close    the connection went away: a crash, a lost network, a denial, or
 *            the application tearing the session down.
 *   expiry   the pending token's clock ran out, long after the connection it
 *            was issued on stopped existing.
 *
 * The first two arrive through nc_auth and both fire the cancelled callback.
 * The third cannot: by then there is no conversation to fire anything, and the
 * pending row outlives every struct here. Expiry is the application's own
 * clock over its own store, and this file has nothing to say about it beyond
 * insisting it exists. A server that only cleans up on cancel leaks every
 * registration a player ever walked away from.
 *
 * WHAT HAS TO BE WIPED
 *
 * nc_auth's existing discipline is that it never retains a secret: a password
 * is copied into the outgoing message and the copy is wiped straight away.
 * The buffers above widen the surface that promise has to cover. A submitted
 * form sits in the server's buffer with a plaintext password in it, and the
 * client's buffer holds the form it is answering. Neither can be left behind
 * for the next conversation to inherit.
 ****************************************************************/

/**
 * End the conversation and wipe it. Wipes the struct and both buffers rather
 * than resetting their lengths, fires the cancelled callback if an interactive
 * method was running, and leaves a struct that is safe to discard.
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

/****************************************************************
 * Messages
 *
 * Four added to the six in nc_auth.c:
 *
 *   MSG_IA_BEGIN   C->S  the chosen method bit, and a token if resuming
 *   MSG_IA_FORM    S->C  title, note, fields, errors
 *   MSG_IA_SUBMIT  C->S  answers, by name
 *   MSG_IA_WAIT    S->C  the slow thing has been started, and what to say
 *   MSG_IA_CANCEL  C->S  abandon the method
 *
 * A cancel is answered with MSG_METHODS, the same reply a finished
 * registration gets, because both land in the same place. A cancel can cross a
 * form already in flight, so a client that has sent one ignores everything
 * until those methods arrive.
 *
 * The reply to a submission is another MSG_IA_FORM, or MSG_IA_WAIT, or the
 * existing MSG_OK or MSG_FAIL. A MSG_IA_WAIT is followed later by whichever of
 * those the server settles on. Registration finishing is MSG_METHODS again,
 * which the client already handles.
 *
 * MSG_IA_BEGIN is a message of its own rather than a field folded into
 * MSG_HELLO, and not only to keep HELLO's meaning intact. It may be sent well
 * into the conversation. A player who mistypes a password, fails, and works
 * out that they never had an account here registers from where they are,
 * without dropping the session and starting over.
 *
 * ONE FORM IS OUTSTANDING AT A TIME
 *
 * A form goes out in reply to a submission, or once a wait has resolved, and at
 * no other time. Nothing here can break that, because there is no call that
 * sends a form: one is produced by returning NC_AUTH_IA_MORE from a callback,
 * or by passing it to nc_auth_server_resume. A server author reaching for an
 * unsolicited re-prompt will not find a function to do it with.
 *
 * The rule governs forms and nothing else. MSG_FAIL is exempt: a server may
 * refuse whenever it decides it is done, including while a form is
 * outstanding, because otherwise a server that wants to stop serving a parked
 * registration has no way to say so. Read as "the server never speaks
 * unsolicited" the invariant would be both false and unimplementable.
 *
 * That invariant is why a form and its answers need nothing to tie them
 * together. The channel is reliable and ordered and the conversation is
 * lock-step, so the only message a client may send after a form is the answer
 * to that form, and there is never a second candidate. ssh's
 * keyboard-interactive has run without a request id for twenty five years on
 * the same reasoning.
 *
 * The failure this looks like it invites is a stale submission crossing a
 * reissued form. Name-keyed answers already prevent it: nothing is positional,
 * so a stale submission cannot land a value in the wrong field, and the server
 * sees names it did not ask for and rejects it. If an identifier is ever needed
 * anyway, a reader skips a tag it has not heard of, so it can be added without
 * breaking an older peer.
 *
 * THE FIELD ENCODING
 *
 * A form is a variable run of variable records, and microser has no repeated
 * field. Rather than teach the generator one, the IDL gains a `stringlist`
 * type: a run of length-prefixed strings packed back to back inside one
 * ordinary bytes field. No new wire type, no change to the tag space, and an
 * older reader still skips it as bytes.
 *
 * Each string carries its own short textual key, so the record structure lives
 * in the strings rather than in a second layer of binary tags:
 *
 *   t=text  n=email  l=Email address  max=64  req=1
 *   t=password  n=pw  l=Password  min=8
 *   t=choice  n=shard  l=Realm  o=Ashen Coast  o=Ravenholt
 *   t=note  l=We sent a code to the address you gave.
 *
 * A record begins wherever `t=` appears, so the list is self-delimiting with
 * no nesting and no separator to get wrong. Numbers are decimal text, which
 * costs a few bytes and buys a form that is readable in a hex dump and maps
 * almost line for line onto the config file the operator wrote it in. An error
 * is `e=` inside the record it belongs to, so the separate errors array in
 * struct nc_form is an API convenience that the encoder folds in.
 *
 * Answers come back the same way, `n=` and `v=`, with `v=` repeated for a
 * multi-choice.
 *
 * NC_AUTH_MAX_MSG at 256 does not survive any of this. The real cap becomes
 * the buffer the caller supplied above.
 ****************************************************************/

/****************************************************************
 * What must be tested
 *
 * tests/test_nc_auth.c already runs both state machines in one process with a
 * queue between them, and every case below fits that harness: no sockets, no
 * timing, no second process. The ordinary paths will get written without being
 * asked for. These are the ones that will not.
 *
 * Cancelling
 *   1. Cancel with a form outstanding. Both ends land back at the method
 *      offer, methods() runs again, cancelled fires once.
 *   2. Cancel during a wait, which is where a player actually presses it.
 *   3. Cancel crossing a form already in flight. The client ignores the form
 *      and still lands at the offer, and neither end keeps state.
 *   4. Cancel, then log in on the same session with an account that already
 *      existed. Abandoning the method must not have cost the login.
 *   5. Cancel with nothing running, and cancel twice. No-ops, one callback at
 *      most.
 *
 * Cleaning up
 *   6. nc_auth_clear mid-registration fires cancelled; a second clear fires
 *      nothing.
 *   7. nc_auth_clear on a struct that never started.
 *   8. After clear, the submitted password appears nowhere in the struct or in
 *      the supplied buffer. Search the bytes rather than trusting a length
 *      field to have been reset.
 *   9. A denied conversation wipes as thoroughly as a cancelled one. Denial is
 *      the path an attacker can drive, so it is the one worth checking.
 *
 * Expiring
 *  10. Resuming with a token the server has already dropped. Denied, no crash,
 *      nothing resurrected.
 *  11. A cancelled registration's token is dead. This is the player who
 *      cancels and then clicks the link that arrived by mail anyway.
 *  12. Cancelling frees the pending slot at once, so the per-address cap
 *      recovers without waiting for a clock.
 *
 * Invariants
 *  13. A submission crossing a reissued form is rejected on the names it
 *      carries, never misbound onto the new form's fields.
 *  14. MSG_FAIL arriving while a form is outstanding is accepted.
 ****************************************************************/

/****************************************************************
 * Not netchan's business
 *
 * Where a server's form comes from. keystore holds the five files that decide
 * trust, and its first paragraph says why they are files an administrator can
 * read: the trust decisions live outside the protocol. A form is not a trust
 * decision. It is configuration, essentially every multiplayer game already has
 * a config format, and netchan is not going to add another one or parse
 * anyone's. What it provides is a way for a server to state a form and a client
 * to receive it.
 *
 * A form the server built wrong is the server operator's bug. Nothing here
 * validates one on the way out or salvages one on the way in. The client drops
 * the connection with an error, an administrator tests their own configuration,
 * and the mistake stays easy to find.
 ****************************************************************/

#endif /* NC_AUTH_INTERACTIVE_H */

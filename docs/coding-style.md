# Coding Style

Formatting and convention rules for C source (`.c`) and headers (`.h`). Match
these rules in new code and when editing existing code.

The document is written to be dropped into any project unchanged. Where it
says "the module prefix" or "the project's standard", fill in what this project
uses. The examples name a fictional module; read them for shape, not for
identifiers.

Vendored third-party code is exempt. It keeps its upstream style so that
updating it stays a clean copy.

A project that adopts this document part way through its life converts a file
when someone is already editing it, rather than in sweeping reformat commits.

## Summary

 * Indent with 4 spaces. No hard tabs.
 * One tag-line file header. No per-file copyright or licence block.
 * Return `0` on success and a negative value on failure, `NULL` for a failed
   pointer.
 * K&R braces for control flow; own-line braces for function definitions.
 * Return type and qualifiers on their own line above the function name.
 * Target 78 columns; up to 100 columns when splitting hurts readability.
 * `snake_case` for names, `UPPER_CASE` for constants, module prefix on
   exports.
 * `struct tag` types. Typedef only opaque handles and function pointers.
 * In a `.c`: own header first, then project headers, then system headers.
 * Trailing comma after the last element of arrays, enums, and initializers.
 * ASCII punctuation only. No trailing whitespace. Final newline.

## Licence and file headers

The project carries one licence file at its root. Source files carry no
copyright block, no licence header, and no author line. A reader who wants the
terms reads the root file, and a relicensing is one edit rather than a sweep.

Every file opens with a single tag line: the filename and a one-line
description. Reference the relevant specification under `docs/` when one
exists.

    /* evq.c : the event queue (see docs/events.md) */

    #include "evq.h"

When the description needs more than a line, use a block comment. It states the
same two things, with the prose in between.

    /*
     * evq.c -- the event queue.
     *
     * Fixed ring of NET_EVQ_SLOTS entries inside the connection object. A push
     * onto a full ring drops the event rather than blocking, because the
     * caller is a packet handler that cannot wait.
     */

Machine-authored files may add `Made by a machine.` to the header prose. That
is a statement of provenance, not a licence, and it does not bring a licence
block with it.

## Include guards

Guard names are the module prefix followed by the upper-cased filename. The
prefix is what keeps a header called `types.h` or `widget.h` from colliding
when it is vendored into a tree that has one of its own.

    /* widget_ext.h : extension widgets */

    #ifndef BD_WIDGET_EXT_H
    #define BD_WIDGET_EXT_H
    ...
    #endif /* BD_WIDGET_EXT_H */

A filename that already begins with the prefix does not repeat it, so
`bd_draw.h` guards with `BD_DRAW_H`.

Do not use `#pragma once`. Older and smaller toolchains do not all have it, and
the portable form costs two lines.

## Includes

In a `.c` file, include its own header first so the header is proven to stand
alone, then project headers, then system headers.

    #include "evq.h"
    #include "net_addr.h"
    #include <string.h>

In a header, include system headers first, then project headers. Include only
what the header's own declarations need, and let the `.c` pull in the rest.

    #include <stdint.h>
    #include <stddef.h>
    #include "net_addr.h"

## Indentation

Indent with 4 spaces per level. Do not use hard tabs.

Nested preprocessor conditionals indent two spaces per level, placed after the
`#` so the directive itself stays in column one.

    #ifdef _WIN32
    #  define WIN32_LEAN_AND_MEAN
    #  include <windows.h>
    #else
    #  include <time.h>
    #endif

## Braces

Opening brace on the same line for control statements; on its own line for
function definitions. A single-statement body may omit its braces.

    int
    net_connect(struct net_conn *c, const struct net_addr *addr)
    {
        if (!c || !addr)
            return -1;
        if (c->state != NET_STATE_NEW) {
            return -1;
        }
        // ...
        return 0;
    }

## Function definitions

Place the return type and any qualifiers on a separate line above the function
name, so the name starts in column one and `grep '^name'` finds the definition.

    static uint8_t *
    pool_ptr(struct net_conn *c, int idx)
    {
        return c->pool_store + (size_t)idx * NET_MAX_MSG;
    }

Prototypes in headers keep the return type on the same line as the name. Align
a column of related prototypes by padding the return type.

    extern int  sock_open(struct sock *s, uint16_t port);
    extern void sock_close(struct sock *s);
    extern long sock_recv(struct sock *s, void *buf, size_t len);

When a parameter list is too long for one line, break after a comma and align
the continuation past the opening parenthesis.

    int net_feed(struct net_conn *c, const void *pkt, size_t len,
                 const struct net_addr *from);

Every parameter takes an explicit type, and an empty parameter list is `void`.
Mark a pointer parameter `const` when the function does not write through it.
An input buffer is `const void *` with a separate `size_t` length. The two
always travel together, and the length is never implied by a sentinel.

## Symbols and linkage

Anything not declared in a header is `static`. There are no unprefixed globals
with external linkage, and no mutable global state: every piece of storage
lives in a caller-owned object passed in as the first parameter.

Name that first parameter for what it is, and keep the same name across the
whole module, so `c` is always the connection and `s` is always the socket.

## Spacing and blank lines

Put spaces around binary operators. Put no space between a function name and
its argument list. Put a space after control keywords. Bind `*` to the name,
not the type.

    next = (c->ev_tail + 1) % NET_EVQ_SLOTS;
    if (len < NET_HDR_SIZE)
        return -1;
    uint8_t *p = buf;

Separate logical groups within a function with blank lines. Keep sequential
assignments to one struct, or to closely related variables, together with no
blank line between them.

## Line width and wrapping

Target 78 columns. Extend to 100 only where breaking a long identifier,
signature, or expression would hurt readability. Break a long expression so the
operator ends the line, and align the continuation with the expression it
belongs to.

    if (data[0] != 'S' || data[1] != 'P' || data[2] != 'A' ||
        data[3] != '2')
        return -1;

## Comments

Divide a long file into sections with an asterisk-bordered block 64 asterisks
wide. These banners are the file's table of contents, so their wording should
match the vocabulary the header uses.

    /****************************************************************
     * Event queue
     ****************************************************************/

Use plain `/* */` for ordinary multi-line comments. Use a leading `/**` for a
function or field doc comment in a public header, placed directly above the
declaration. Write prose, not `@param` tags; the first sentence is the summary.

    /** The version as one comparable integer, e.g. 0.5.0 is 500. Use it to
     *  compile against more than one release: NET_VERSION >= 500. */

Use `//` for short inline and end-of-line comments. Use `/* */` for comments on
`#endif` and preprocessor lines.

    c->state = NET_STATE_CLOSED; // no further sends are attempted
    #endif /* BD_WIDGET_EXT_H */

Comment the why, not the what. A comment earns its place when it records a
format constraint, a portability workaround, or a decision the code alone
cannot show.

## Naming

Use `snake_case` for functions, variables, struct tags, and enum types. Use
`UPPER_CASE` for constants, enum values, and macros.

Prefix every exported symbol with its module name, and use the upper-cased
prefix on the module's macros and enum values. One module owns one prefix, and
the prefix does not change between the header, the source, and the tests.

    void net_close(struct net_conn *c);
    #define NET_MAX_CHAN 16

Static helpers take no prefix, because the file is already their namespace.

    static int  pool_get(struct net_conn *c);
    static void ev_push(struct net_conn *c, int type);

A constant that describes an external format rather than the API keeps the
format's prefix even inside a module named something else, so a reader can tell
a wire constant from a tunable.

### Abbreviations

Use the short form, consistently. Do not invent a second spelling of a word
that is already on the list, and do not abbreviate anything that is not.

| Full word                     | Abbreviation |
| ----------------------------- | ------------ |
| address                       | addr         |
| acknowledgement               | ack          |
| allocator, allocation         | alloc        |
| buffer                        | buf          |
| channel                       | chan         |
| configuration                 | cfg          |
| context                       | ctx          |
| destination                   | dst          |
| event                         | ev           |
| fragment                      | frag         |
| graphics                      | gfx          |
| header                        | hdr          |
| index                         | idx          |
| initialize                    | init         |
| interface definition language | idl          |
| length                        | len          |
| message                       | msg          |
| packet                        | pkt          |
| public key                    | pk           |
| receive                       | recv         |
| rectangle                     | rect         |
| secret key                    | sk           |
| sequence                      | seq          |
| source                        | src          |
| vector                        | vec          |

## Types

Declare aggregates as `struct tag` and use them as `struct tag` at the point of
use. A reader should be able to see from the declaration that a value is an
aggregate, and a forward declaration should not need a header.

    struct net_conn *c;

    void net_close(struct net_conn *c);

Typedef is for two cases. The first is an opaque handle, where the definition
stays private to the `.c` file and the caller has nothing to look inside.

    /* in the header */
    typedef struct net_session net_session;

    net_session *net_session_open(void);

    /* in the source, where the caller cannot reach it */
    struct net_session {
        // ...
    };

The second is a function pointer type, where the alias genuinely improves the
declaration.

    typedef void (*net_send_cb)(void *ctx, const void *msg, size_t len);

Do not typedef a struct the caller can see inside, and do not typedef an enum
or a union at all.

Use `const` on the left, in the ordinary C spelling: `const uint8_t *p`.

Use fixed-width types from `<stdint.h>` for anything that reaches a file or the
network. Use `int` for a loop counter and `size_t` for a length.

An anonymous enum is the right tool for a set of related integer constants that
share no variable type, such as state values or event kinds.

## Return values

Functions return `0` on success and a negative value on failure. Where the
caller needs to distinguish causes, define a negative error enum for the module
and return one of its values rather than a bare `-1`.

    enum {
        NET_OK        =  0,
        NET_ERR       = -1,
        NET_ERR_NOMEM = -2,
        NET_ERR_AGAIN = -3,
    };

Pointer-returning functions return `NULL` on failure. A function that returns a
byte count returns a non-negative count, and a negative error code on failure.
Never return a value the caller has to interpret by consulting `errno`.

Do not define `OK` or `ERR` macros with no prefix, and do not return a `bool`
from a function that can fail in more than one way.

## If and else

Use guard clauses. Validate arguments and reject impossible states at the top
of the function, then let the body run at one level of indentation.

    static int
    process_data(struct net_conn *c, const uint8_t *p, size_t len)
    {
        if (len < NET_HDR_SIZE)
            return NET_ERR_PROTO;
        if (!c->chan[id].open)
            return NET_ERR_CLOSED;

        // the real work, unindented
    }

Do not put an `else` after a branch that returns.

## Switch

Case labels sit at the same indentation as the `switch`, and the case body is
indented one level. A case that declares variables gets its own braces.

    switch (c->state) {
    case NET_STATE_CONNECTING:
    case NET_STATE_CONNECTED:
        c->state = NET_STATE_CLOSED;
        ev_push(c, NET_EV_DISCONNECTED);
        break;
    default:
        break;
    }

A switch over a value that arrived from outside the program handles every case
it accepts and rejects the rest in `default`. Mark a deliberate fall-through
with a comment.

## Trailing commas

End every array, enum, and initializer list with a trailing comma after the
last element, so adding an entry touches one line.

    enum {
        NET_RELIABLE,
        NET_UNRELIABLE,
        NET_STREAM,
    };

## Macros

Keep object-like constants `UPPER_CASE` and give them the module prefix.

    #define NET_MAX_CHAN 16
    #define NET_MAX_MSG  2048

Wrap a multi-statement macro in `do { } while (0)`, and parenthesize every
argument and the whole result. Prefer a `static` function to a function-like
macro whenever the compiler can inline it. Macros are for what a function
cannot do, such as capturing an expression's text.

    #define TEST(name) \
        do { \
            tests_run++; \
            printf("  %-50s ", name); \
            fflush(stdout); \
        } while (0)

## Memory

State the allocation policy in the module's header and hold to it. A module
that allocates once at open and never again is worth more than one that is
merely careful, because the bound can be checked by reading the struct.

Size fixed storage with named constants rather than literals, so the bound has
a name a reader can search for. Do not add an allocation to a hot path, and do
not add one at all without a note saying what bounds it.

Null a pointer after freeing it. Zero a buffer that held key material with an
explicit wipe rather than `memset` alone, which the compiler is allowed to
elide.

## Portability

Write to the standard the project's build selects, with no compiler extensions,
and keep to the intersection of the toolchains it supports. Where the project
targets a small or old toolchain, that intersection is narrow: no VLAs, no
`alloca`, no statement expressions, no nested functions.

Never assume that `int` is 32 bits, that pointers and integers are
interchangeable, or that the host is little-endian. Encode and decode external
formats byte by byte with explicit shifts. Do not cast a buffer to a struct,
and do not use bitfields for a packed format.

## Platform guards

Platform-specific code lives in its own file, named for the platform, and is
reached through the same interface as every other backend. Layering is the real
portability mechanism: when the portable core never names a platform facility,
a new platform is a new file rather than a new `#ifdef`.

Where a guard has to appear inside a shared file, keep it small, keep it near
the top, and give it a comment saying why the platform needs it.

    /* Monotonic clock: clock_gettime on POSIX, GetTickCount64 on Windows. */
    #ifdef _WIN32
    #  include <windows.h>
    #else
    #  include <time.h>
    #endif

## Tests

Each test binary is one `.c` file named `<subject>_test.c`, with no framework
beyond a `TEST` / `PASS` / `FAIL` macro trio. A test prints one line per
assertion and a summary, exits `0` only when everything passed, and never
depends on the network, the clock, or the order of other tests.

The file header comment states what the test covers, in a sentence a reader can
check against the assertions.

## Non-C files

A build system file that is fetched or generated is not edited by hand. Record
where it came from and how to refresh it.

Shell scripts are POSIX `sh`, not bash, and pass `shellcheck`. Awk programs
live in their own `.awk` file invoked with `-f`, never inside a shell quoted
string, and pass `gawk --lint`.

Avoid locale-dependent constructs in both. Compare characters with `index()`
against an explicit set rather than with a range like `c >= "A" && c <= "Z"`,
which follows the locale's collation order and silently changes meaning on
another machine.

Do not add Python to the build.

## Punctuation

Use ASCII punctuation in all source, headers, and documentation: `--` for a
dash, `"quotes"` for quotation marks, `...` for an ellipsis. Unicode is
acceptable only inside program data, such as a user-facing string.

Write documentation in plain sentences. Use commas for brief pauses, or split a
long sentence into two.

## Whitespace

Leave no trailing whitespace. End each file with a single newline and no extra
blank lines.

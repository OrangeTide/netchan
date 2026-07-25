# netchan Coding Style

Formatting and convention rules for C source (`.c`) and headers (`.h`) in this
project. Match these rules in new code and when editing existing code.

This style is shared with the author's other C projects, so a reader moving
between them sees one set of habits. Existing netchan source predates the
document and does not match it everywhere. Convert a file when you are already
editing it, rather than in sweeping reformat commits.

`third_party/` is excluded. Vendored code keeps its upstream style so that
updating it stays a clean copy, and the same applies to any vendored event loop
or support file under `examples/`.

## Summary

 * Indent with 4 spaces. No hard tabs.
 * Two-line file header: a tag line, then the public domain line.
 * Return `0` on success and a negative code on failure, `NULL` for a failed
   pointer.
 * K&R braces for control flow; own-line braces for function definitions.
 * Return type and qualifiers on their own line above the function name.
 * Target 78 columns; up to 100 columns when splitting hurts readability.
 * `snake_case` for names, `UPPER_CASE` for constants, module prefix on
   exports.
 * `struct tag` types, not typedefs.
 * In a `.c`: own header first, then project headers, then system headers.
 * Trailing comma after the last element of arrays, enums, and initializers.
 * ASCII punctuation only. No trailing whitespace. Final newline.

## Language and portability

The code is C11 with no compiler extensions and no dependencies beyond libc.
It has to build with gcc, clang, MSVC, Emscripten, and Open Watcom for 16-bit
DOS, so keep to the intersection:

 * No VLAs, no `alloca`, no statement expressions, no nested functions.
 * No `long long` arithmetic in `microchan/`, which targets 16-bit targets.
 * Fixed-width types from `<stdint.h>` for anything that reaches the wire.
 * Never assume `int` is 32 bits, that pointers and integers are
   interchangeable, or that the host is little-endian.

Encode and decode wire fields byte by byte with explicit shifts. Do not cast a
buffer to a struct and do not use bitfields for a packed format.

## File header

Every file opens with two lines: the filename with a one-line description, and
the public domain notice. Reference the relevant `docs/` specification when one
exists.

    /* netchan.c : multiplexed UDP channels for game networking */
    /* PUBLIC DOMAIN (CC0-1.0) */

    #include "netchan.h"

The project carries one `LICENSE` (CC0-1.0) at its root, and the second header
line is the only per-file licence marking. Do not add a copyright block or an
author line. A machine-authored file may spell the second line
`/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */`.

When the description needs more than a line, use a block comment instead, with
the same two pieces of information and the prose in between.

    /*
     * microser_test.c -- round-trip and edge tests for the microser codecs.
     *
     * PUBLIC DOMAIN (CC0-1.0)
     */

In a header, the include guard follows the header lines. Guard names are the
upper-cased filename with no extra project prefix, so `nc_udp.h` guards with
`NC_UDP_H`.

    /* nc_udp.h : UDP socket transport for netchan */
    /* PUBLIC DOMAIN (CC0-1.0) */

    #ifndef NC_UDP_H
    #define NC_UDP_H
    ...
    #endif /* NC_UDP_H */

Do not use `#pragma once`. The Watcom build needs the portable form.

## Includes

In a `.c` file, include its own header first so the header is proven to stand
alone, then project headers, then system headers.

    #include "nc_crypto.h"
    #include "monocypher.h"
    #include <string.h>

In a header, include system headers first, then project headers. Include only
what the header's own declarations need, and let the `.c` pull in the rest.

    #include <stdint.h>
    #include <stddef.h>
    #include "nc_addr.h"

## Indentation

Indent with 4 spaces per level. Do not use hard tabs.

Nested preprocessor conditionals indent two spaces per level, placed after the
`#` so the directive stays in column one.

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
    netchan_connect(struct netchan_conn *c, const struct nc_addr *addr)
    {
        if (!c || !addr)
            return NETCHAN_ERR;
        if (c->state != NETCHAN_STATE_NEW) {
            return NETCHAN_ERR;
        }
        // ...
        return NETCHAN_OK;
    }

## Function definitions

Place the return type and any qualifiers on a separate line above the function
name, so the name starts in column one and `grep '^name'` finds the definition.

    static uint8_t *
    pool_ptr(struct netchan_conn *c, int idx)
    {
        return c->pool_store + (size_t)idx * NC_MAX_MSG;
    }

Prototypes in headers stay on one line, with the return type on the same line
as the name. Align a column of related prototypes by padding the return type.

    extern int  nc_udp_open(struct nc_udp *u, uint16_t port);
    extern void nc_udp_close(struct nc_udp *u);

When a parameter list is too long for one line, break after a comma and align
the continuation past the opening parenthesis.

    int netchan_feed(struct netchan_conn *c, const void *pkt, size_t len,
                     const struct nc_addr *from);

Every parameter takes an explicit type, and an empty parameter list is `void`.
Mark a pointer parameter `const` when the function does not write through it.
An input buffer is `const void *` with a separate `size_t` length; the two
always travel together and the length is never implied by a sentinel.

## Symbols and linkage

Anything not declared in a header is `static`. There are no unprefixed globals
with external linkage, and no mutable global state at all: every piece of
storage lives in a caller-owned object passed in as the first parameter.

Name that first parameter for what it is (`c` for a connection, `ch` for a
channel, `u` for a UDP context) and keep the same name across the whole module.

## Spacing and blank lines

Put spaces around binary operators. Put no space between a function name and
its argument list. Put a space after control keywords. Bind `*` to the name,
not the type.

    next = (c->ev_tail + 1) % NC_EVENT_QUEUE;
    if (len < NC_HDR_INIT_SIZE)
        return NETCHAN_ERR_PROTO;
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

Divide non-function sections with an asterisk-bordered block 64 asterisks wide.
These are the table of contents of a long file, so the banner text should match
the vocabulary the header uses.

    /****************************************************************
     * Event queue
     ****************************************************************/

Use plain `/* */` for ordinary multi-line comments. Use a leading `/**` for a
function or field doc comment in a public header, placed directly above the
declaration. Write prose, not `@param` tags; the first sentence is the summary.

    /** The version as one comparable integer, e.g. 0.5.0 is 500. Use it to
     *  compile against more than one release: NETCHAN_VERSION >= 500. */

Use `//` for short inline and end-of-line comments. Use `/* */` for comments on
`#endif` and preprocessor lines.

    c->state = NETCHAN_STATE_CLOSED; // no further sends are attempted
    #endif /* NETCHAN_H */

Comment the why, not the what. A comment earns its place when it records a
wire-format constraint, a portability workaround, or a decision that the code
alone cannot show.

## Naming

Use `snake_case` for functions, variables, struct tags, and enum types. Use
`UPPER_CASE` for constants, enum values, and macros.

Prefix every exported symbol with its module name. The project's prefixes are:

| Prefix     | Belongs to                                        |
| ---------- | ------------------------------------------------- |
| `netchan_` | the protocol core's public API                    |
| `nc_`      | shared types and the transport and crypto layers  |
| `mc_`      | microchan, the 16-bit-capable second library      |
| `microser_`| the IDL runtime                                   |

Static helpers take no prefix (`pool_get`, `ev_push`, `rd16`). Enum values and
macros carry the module prefix upper-cased (`NETCHAN_ERR_AGAIN`,
`NC_FRAME_DATA`).

Internal frame and header constants belong to the wire format and use the
`NC_` prefix even inside a `netchan_` module, because they describe the
protocol rather than the API.

### Abbreviations

Use the short form, consistently, and do not invent a second spelling of the
same word.

| Full word                     | Abbreviation |
| ----------------------------- | ------------ |
| address                       | addr         |
| acknowledgement               | ack          |
| buffer                        | buf          |
| channel                       | chan         |
| configuration                 | cfg          |
| context                       | ctx          |
| destination                   | dst          |
| event                         | ev           |
| fragment                      | frag         |
| header                        | hdr          |
| index                         | idx          |
| initialize                    | init         |
| interface definition language | idl          |
| length                        | len          |
| message                       | msg          |
| packet                        | pkt          |
| public key                    | pk           |
| receive                       | recv         |
| secret key                    | sk           |
| sequence                      | seq          |
| source                        | src          |

Do not abbreviate anything not on this list. `connection` stays `connection`
in prose and `conn` only where it already appears in a type name.

## Types

Declare aggregates as `struct tag` and use them as `struct tag` at the point of
use. Do not typedef a struct, union, or enum. A reader should be able to see
from the declaration that a value is an aggregate.

    struct netchan_conn *c;

Typedef is reserved for function pointer types, where the alias genuinely
improves the declaration.

    typedef void (*nc_auth_send_cb)(void *ctx, const void *msg, size_t len);

Use `const` on the left, in the ordinary C spelling: `const uint8_t *p`.

Anonymous enums are the right tool for a set of related integer constants that
share no variable type, which is how the protocol states and event kinds are
written.

## Return values

Functions return `0` on success and a negative value on failure. Where the
caller needs to distinguish causes, return one of the module's negative error
codes rather than a bare `-1`.

    enum {
        NETCHAN_OK        =  0,
        NETCHAN_ERR       = -1,
        NETCHAN_ERR_NOMEM = -2,
        NETCHAN_ERR_AGAIN = -3,
    };

Pointer-returning functions return `NULL` on failure. A function that returns
a byte count returns a non-negative count, and a negative error code on
failure. Never return a value that the caller has to compare against `errno`.

## If and else

Use guard clauses. Validate arguments and reject impossible states at the top
of the function, then let the body run at one level of indentation.

    static int
    process_data(struct netchan_conn *c, const uint8_t *p, size_t len)
    {
        if (len < NC_HDR_FULL_SIZE)
            return NETCHAN_ERR_PROTO;
        if (!c->chan[id].open)
            return NETCHAN_ERR_CLOSED;

        // the real work, unindented
    }

Do not put an `else` after a branch that returns.

## Switch

Case labels sit at the same indentation as the `switch`, and the case body is
indented one level. A case that declares variables gets its own braces.

    switch (c->state) {
    case NETCHAN_STATE_CONNECTING:
    case NETCHAN_STATE_CONNECTED:
        c->state = NETCHAN_STATE_CLOSED;
        ev_push(c, NETCHAN_EV_DISCONNECTED, NULL);
        break;
    default:
        break;
    }

A switch over an enumerated wire value handles every case it accepts and
rejects the rest in `default`. Mark deliberate fall-through with a comment.

## Trailing commas

End every array, enum, and initializer list with a trailing comma after the
last element, so adding an entry touches one line.

    enum {
        NETCHAN_RELIABLE,
        NETCHAN_UNRELIABLE,
        NETCHAN_STREAM,
    };

## Macros

Keep object-like constants `UPPER_CASE`, and give them the module prefix.

    #define NC_MAX_CHAN   16
    #define NC_MAX_MSG    2048

Wrap a multi-statement macro in `do { } while (0)` and parenthesize every
argument and the whole result. Prefer a `static` function to a function-like
macro whenever the compiler can inline it; macros are for the cases where a
function cannot do the job, such as the test harness.

    #define TEST(name) \
        do { \
            tests_run++; \
            printf("  %-50s ", name); \
            fflush(stdout); \
        } while (0)

## Memory

The core allocates once, in `netchan_open`, and never again. Every buffer,
queue, and pool is sized at compile time by a `NC_` constant and lives inside
the connection object. Do not add a `malloc` to a hot path, and do not add one
at all without a note in the header explaining the bound.

`microchan/` goes further: one allocation per connection and nothing after
that. Code there must hold to it.

Null a pointer after freeing it. Zero any buffer that held key material with an
explicit wipe, not `memset` alone, so the compiler cannot elide it.

## Platform guards

Platform-specific code lives in its own file where possible, named for the
platform, and reached through the same interface as every other backend.

Where a guard has to appear inside a shared file, keep it small, keep it near
the top, and give it a comment saying why the platform needs it.

    /* Monotonic clock: clock_gettime on POSIX, GetTickCount64 on Windows. */
    #ifdef _WIN32
    #  include <windows.h>
    #else
    #  include <time.h>
    #endif

Portable logic stays outside such guards. The layering is the real portability
mechanism: the protocol core never names a socket, so a new platform is a new
transport file rather than a new `#ifdef`.

## Tests

Each test binary is one `.c` file under `tests/`, named `<subject>_test.c`,
with a `TEST` / `PASS` / `FAIL` macro trio and no framework. A test prints one
line per assertion and a summary, exits `0` only when everything passed, and
never depends on the network, the clock, or the order of other tests.

The file header comment states what the test covers, in a sentence a reader can
check against the assertions.

## Non-C files

`GNUmakefile` is vendored from modular-make and is not edited by hand. Per
directory build rules live in that directory's `module.mk`.

Shell scripts are POSIX `sh`, not bash, and pass `shellcheck`. Awk programs
live in their own `.awk` file invoked with `-f`, never inside a shell quoted
string, and must pass `gawk --lint`. Avoid locale-dependent constructs in both:
compare characters with `index()` against an explicit set rather than with a
range like `c >= "A" && c <= "Z"`, which follows the locale's collation order.

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

## Relation to the sibling projects

netchan, smoltrek, and birdie-gui share the same habits, and a reader moving
between them should not have to relearn anything. What they all do:

 * One indent level per nesting level, ASCII punctuation, no trailing
   whitespace, a final newline.
 * `snake_case` names, `UPPER_CASE` constants, a module prefix on every
   exported symbol, no prefix on `static` helpers.
 * Return type on its own line above the function name in a definition, and on
   the same line in a prototype.
 * K&R braces for control flow, own-line braces for function definitions.
 * `#ifndef` include guards, never `#pragma once`.
 * Own header first in a `.c`, then project headers, then system headers.
 * Everything not in a header is `static`, and no mutable globals.
 * A caller-owned context object as the first parameter, not hidden state.
 * A per-file tag line naming the file and what it does, with the licence line
   beside it, and one licence file at the root.

Where they differ, and what netchan does:

| Point           | netchan            | smoltrek        | birdie-gui           |
| --------------- | ------------------ | --------------- | -------------------- |
| Indent          | 4 spaces           | 4 spaces        | hard tabs            |
| Licence         | CC0-1.0, per file  | MIT-0, root only| CC0-1.0, per file    |
| Header form     | two tag lines      | one tag line    | block comment        |
| Aggregates      | `struct tag`       | `struct tag`    | typedef'd            |
| Section banner  | asterisk box       | asterisk box    | dashed rule          |
| Guard name      | file name          | file name       | prefix + file name   |
| Failure return  | negative enum code | `-1`            | `0` for invalid      |

Follow this document inside netchan. Do not import birdie-gui's typedef'd
aggregates or its banner form here, and do not push netchan's error enum into
a project that has no use for it.

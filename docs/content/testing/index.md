---
title: Testing and analysis
weight: 10
abstract: Five layers of checking, from unit tests to a fuzzer, and what each one has actually found.
category: manual
---

What netchan checks about itself, how to run each check, and what each one
has actually found. The short version: five layers, from ordinary unit tests
to a fuzzer, each aimed at a class of defect the others do not reach.

## The layers

| Check | Command | Finds |
|---|---|---|
| Test suite | `make run-tests` | behaviour, on 20 binaries |
| Sanitizers | `make run-tests CFLAGS=...` | memory and UB at runtime |
| Fuzzing | see below | parser crashes on hostile input |
| Static analysis | `make analyze` | leaks and misuse on unexercised paths |
| Style | `make lint` | drift from `docs/coding-style.md` |

None of them subsumes another. The suite proves the protocol does what it is
supposed to; the sanitizers watch the same runs for memory errors; the fuzzer
supplies inputs no test would think to write; the analyzer reaches branches no
run takes; the linter is about consistency, not correctness.

## The test suite

`make run-tests` builds and runs 20 binaries covering the protocol core,
channels under loss and reordering, flow control, the WebSocket codec, the
IDL codecs, the encrypted decorator, the login state machine and its
interactive method, the discovery packet, the on-disk key formats, the UDP
transport over real loopback sockets, microchan, and the game example's
network layer.

Most of it is socketless on purpose: the two halves run in one process with
buffers between them, which is what lets the same tests run under wasm and on
16-bit DOS. `nc_udp_test` is the exception and needs real sockets, and two
examples are driven end to end by shell: the chat server through a client
disconnecting, and the beacon demo through a server announcing itself and
being found.

`make -f Makefile.simple check` is a second, smaller path: it builds the core
with nothing but `cc` and runs 15 checks, which is how the no-dependencies
claim stays honest.

## Sanitizers

```sh
make CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
     LDFLAGS="-fsanitize=address,undefined"
make run-tests CFLAGS="..." LDFLAGS="..."
```

AddressSanitizer and UndefinedBehaviorSanitizer run the whole suite in CI on
every push, with `detect_leaks=1` and `halt_on_error=1`. They are also what
makes the fuzzer worth running: a fuzzer without a sanitizer only notices
crashes, while a fuzzer with one notices the reads and writes that would have
become crashes on a different allocator.

## Fuzzing

`netchan_feed` is the largest of the library's untrusted surfaces. Every byte
a peer can send arrives through it, and the frame parsers, fragment
reassembly, the reorder buffer and the channel table all run on data nobody
has validated. `tests/fuzz_netchan_feed.c` is its harness.

Two more have since joined it and have no harness yet. `nc_beacon_parse` reads
a packet from anyone on the local network, which is a lower bar than netchan's
own, since a beacon needs no handshake first. The form decoder inside
`nc_auth_ia.c` reads a message from a peer that is by definition not
authenticated, because the whole point of it is to run before there is an
account. Both are better fuzzing candidates than `netchan_feed` on the
argument this page already makes, and both are unfuzzed today.

The harness drives a whole session per input rather than feeding one datagram
to a fresh connection, because the interesting bugs need state. It feeds the
input, drains the event queue, reads any channel that delivered, services the
timers, and builds a reply. A reassembly bug cannot appear until something has
opened a channel and half filled a fragment.

It is deterministic where it matters. `netchan_service` takes the time as an
argument and the fuzzer supplies it from the input, so a crash replays from
its corpus file rather than depending on when it ran.

To fuzz for real:

```sh
clang -fsanitize=fuzzer,address,undefined -Isrc -Itransport \
    tests/fuzz_netchan_feed.c src/netchan.c -o fuzz_feed
./fuzz_feed tests/fuzz_corpus
```

Built with `-DFUZZ_STANDALONE` instead, the same file grows a `main()` that
replays files, which is what `make run-tests` runs. That keeps the corpus
useful to every compiler in the matrix, not only to clang, and it is what
turns a crashing input into a regression test: drop the file the fuzzer
produced into `tests/fuzz_corpus` and the suite replays it forever after.

CI fuzzes for two minutes on every push, on top of the replay, and uploads any
crashing input as an artifact.

### The corpus

`tests/fuzz_corpus` holds 133 seeds in 2.5 kB. They are a size-bounded
minimization of a live run, chosen with `-merge=1 -max_len=32`, and they hold
855 of the 895 coverage edges the unbounded corpus reached. Seeds exist so a
fresh run starts from real protocol structure rather than rediscovering the
5-byte header from noise.

## Static analysis

```sh
make analyze
```

Builds the tree under `gcc -fanalyzer` and fails on a warning in the project's
own code. The analyzer walks execution paths rather than matching patterns,
which is why it reports what a review and a plain warning flag both miss: a
descriptor leaked down one branch, a value used before the guard that
validates it, a double free.

Vendored code is exempt, the same rule the linter follows. `examples/iox`
carries four warnings upstream, and fixing them in a copy would only make the
next update harder.

Two notes for anyone running it by hand. The flags omit `-std=` deliberately,
because selecting strict ISO C hides the POSIX declarations this tree uses and
buries the output under implicit-declaration errors. And the analyzer follows
calls, so an unreachable function is not analyzed deeply: dead code reports as
unused rather than as buggy.

## Results

As of 0.9.0, on this tree:

- The suite passes 191 assertions across its 20 binaries, and
  `Makefile.simple` passes its 15.
- Sanitizers are clean over the whole suite.
- The fuzzer ran 522,458 executions at about 8,500/second and found no crash.
  It added 1,641 new coverage units along the way, so it was exploring rather
  than spinning.
- `gcc -fanalyzer` found two real defects when it was first run, both since
  fixed: `bind_lo` in `nc_udp_test.c` called `bind()` on the result of
  `socket()` without checking it, and `test_secure_link.c` leaked a descriptor
  on two failure paths where one of two sockets opened and the other did not.
- Every check runs on Linux with gcc and clang, macOS with clang, Windows via
  mingw and wine, wasm32 via emscripten, and 16-bit DOS via Open Watcom.

Worth stating plainly: the two analyzer findings were both in test code, and
the fuzzer has not yet found anything. That is a weaker claim than it sounds.
It means the parsers survive the inputs a fuzzer reaches in minutes, not that
they are correct. Bounded model checking of the frame parsers is the next
thing that would strengthen it, by exhausting the input space up to a depth
rather than sampling it.

## What is not covered

The socket layer and the OS interaction below it, since `nc_udp` is mostly a
thin call into the kernel and the core never names a socket. Portability is
covered by building on five toolchains rather than by any check here. And
nothing verifies the protocol design itself: the tests demonstrate that a
session works, not that no reachable state deadlocks.

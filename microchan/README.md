# microchan

The same idea as [netchan](../README.md), sized for a machine that does not
have a megabyte to spare. Multiplexed reliable and unreliable channels over an
unreliable datagram transport, with one allocation per connection and none
after it, every buffer inside that connection fixed at compile time, and a
core small enough to sit in a 16-bit large-model DOS binary next to a whole
game.

It runs over IPX on MS-DOS and over UDP on a host. The UDP backend is not an
afterthought: it is how the core and the game get developed and tested with
`gcc` on a normal machine, which is the only reason any of this is testable.

## It Is Not a Netchan Build Option

microchan is a separate library that happens to share a repository. The wire
formats differ, the APIs differ, and `struct mc_addr` and `struct nc_addr`
pack their bytes differently while meaning roughly the same thing. Nothing
links both, and nothing should try.

What was given up, and what it bought:

| netchan | microchan | why |
|---|---|---|
| allocates per connection, per channel, and per queued message | one allocation per connection, everything inside it fixed by a macro | one failure path instead of many, and a footprint you can read off the header |
| selective ack, per-message | Go-Back-N over an 8-message window | a window fits in a byte; a sack bitmap and its bookkeeping do not |
| reliable, unreliable, stream | reliable, unreliable | a stream needs reassembly buffers nobody had room for |
| negotiated channels with content types | channel id is open order | no negotiation is no state and no strings |
| connection migration, redirect, stats | connect, exchange, disconnect | each feature is bytes in a struct that exists four times over |

The tunables live at the top of `src/microchan.h` and are the dial:
`MC_MTU`, `MC_WINDOW`, `MC_MAX_CHAN`, `MC_RECVQ`, `MC_UNREL_TXQ`, and
`MC_MAX_CONN`. The defaults are sized for a four-player game with the 546-byte
payload IPX guarantees on any medium. Override them with `-D`.

Both libraries kept the seam that matters. The core still never names a
socket: datagrams arrive through `mc_feed()`, leave through `mc_send_next()`,
and carry an opaque `struct mc_addr` the core copies but never reads. That is
what lets one core speak IPX in DOSBox and UDP under `gcc`.

## Layout

| Directory | What it is |
|---|---|
| `src/` | `microchan.c/h`, the core, and `mc_addr.h`, the transport seam |
| `transport/` | `mc_udp` for the host, `mc_ipx` for 16-bit DOS |
| `tests/` | host loopback tests over real UDP sockets |
| `examples/` | `mcdemo`, the game, and the DOSBox harness scripts |

## Build

The host build is part of the repository's normal `make`:

```sh
make                         # from the repository root
make run-tests
```

It produces `test_microchan`, `mcdemo`, `thor`, `test_game`, and `test_gnet`.
`mc_ipx.c` is absent from it, because real-mode 16-bit code does not compile
on a host.

The DOS build is Open Watcom's, and lives in this directory:

```sh
export WATCOM=/path/to/open-watcom
export PATH="$WATCOM/binl:$PATH"
cd microchan && wmake
```

That writes `_dos/mcdemo.exe`, `_dos/ipxtest.exe`, and `_dos/thor.exe`,
16-bit, large model, clean at `-w4`. GNU make reads the `module.mk` tree;
Open Watcom's `wmake` reads `makefile`. They share the sources and nothing
else, and their objects land in different directories.

## Using It

```c
struct microchan *c = mc_open(1);        /* 1 = server */
mc_connect(c, &addr);                     /* client side */

struct mc_chan *rel = mc_chan_open(c, MC_RELIABLE);
struct mc_chan *unrel = mc_chan_open(c, MC_UNRELIABLE);
mc_write(rel, buf, len);

/* the application owns the transport */
uint32_t id = mc_peek_id(pkt, len);       /* 0 means a new connection */
mc_feed(c, pkt, len, &from);

struct mc_event ev;
while (mc_poll(c, &ev))
    if (ev.type == MC_EV_DATA)
        mc_read(ev.ch, buf, sizeof buf);
```

Both peers must open the same channels in the same order, because the channel
id *is* the open order. There is no negotiation, which is the point: no
negotiation is no state to keep and no strings to compare.

A server is an array of `struct microchan`, one per peer, and `mc_peek_id()`
tells you which one an arriving datagram belongs to. An id of 0 is a new
connection attempt.

## Playing thor Over IPX

`thor` is a four-player, server-authoritative game in a 40x25 text screen,
and it is the reason the variant exists. On the host it plays over UDP:

```sh
_out/*/bin/thor s       # host
_out/*/bin/thor         # join
```

On DOS it plays over IPX, which needs DOSBox and an IPX-over-UDP relay that
speaks the DOSBox tunnel protocol, such as `ipxrelay`.

1. Start the relay, e.g. on UDP 19900.
2. Give each DOSBox an IPX-enabled config that mounts `microchan/_dos` as
   `C:` and runs `ipxnet connect <relay> 19900` before `thor.exe`.
   `examples/dosbox.cnf` is a working starting point.
3. Start the host (`thor.exe s`) first, then the joiners. Each joiner finds
   the host with an IPX broadcast.

A fixed `cycles` value matters. `cycles=max` makes two co-running DOSBox
instances starve each other.

### Controls

A keyboard twin-stick shooter: one hand steers, the other aims.

- **Arrows** move in eight directions (hold to keep moving)
- **W A S D** fire north, west, south, east, independent of where you are
  moving; combine two for a diagonal
- **Z** or **Space** fire in the direction you last moved
- **Enter** opens the chat line; Enter sends, Esc cancels
- **Q** or **Esc** quits

### Harness Scripts

These automate the relay and a headless two-instance match, and only ever
manage the processes they start. They need `WATCOM` set.

```sh
examples/scripts/relay.sh start|stop|restart|status
sh examples/scripts/ipxtest.sh pair ipxtest.exe s   # reliability over IPX
sh examples/scripts/playtest.sh                      # headless match, dumps both screens
```

`thor.exe` takes a `dump` argument that writes the current 40x25 screen to a
text file once a second, which is how the headless harness captures a match.

## Licence

Made by a machine. PUBLIC DOMAIN (CC0-1.0). See [../LICENSE](../LICENSE).

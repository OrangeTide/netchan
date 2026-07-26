/* fuzz_netchan_feed.c : drive netchan_feed with attacker-controlled bytes */

/*
 * netchan_feed is the whole untrusted surface of the core: every byte a peer
 * can send arrives through it, and everything below it (the frame parsers,
 * fragment reassembly, the reorder buffer, the channel table) runs on data
 * nobody has validated. That is what this feeds.
 *
 * One connection per input, torn down at the end, so a leak or a
 * use-after-free shows up under AddressSanitizer rather than being absorbed
 * by the next iteration. The session is driven the way a real program drives
 * it, because a bug that needs a channel open and a fragment half assembled
 * will not appear from feeding one datagram into a fresh connection:
 *
 *   feed it            parse the datagram
 *   poll               drain the event queue, as an application must
 *   chan_read          empty any channel that delivered, freeing pool buffers
 *   service            run the timers, which is where retransmission lives
 *   send_next          build a reply, which exercises the send path too
 *
 * The clock is not read from the machine. netchan_service takes the time as
 * an argument, and the fuzzer supplies it from the input, so a crash replays
 * from its corpus file rather than depending on when it ran. netchan_feed
 * still stamps last_recv_ms from the real clock internally, which affects
 * only timeout arithmetic, not parsing.
 *
 * Build:
 *   clang -fsanitize=fuzzer,address,undefined -Isrc -Itransport \
 *       tests/fuzz_netchan_feed.c src/netchan.c -o fuzz_feed
 *
 * Replay a corpus without libFuzzer (what CI runs):
 *   cc -DFUZZ_STANDALONE -Isrc -Itransport \
 *       tests/fuzz_netchan_feed.c src/netchan.c -o fuzz_feed
 *   ./fuzz_feed corpus/
 */

#include "netchan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * The first two bytes steer the run and the rest is the datagram. Spending
 * input on the shape of the session rather than on the packet is deliberate:
 * a client and a server take different branches through the frame parsers,
 * and an unaccepted server ignores most of them.
 */
#define CTRL_BYTES 2

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct netchan_conn *c;
    struct netchan_event ev;
    struct nc_addr from;
    uint8_t out[2048];
    uint32_t now;
    int server, accept;

    if (size < CTRL_BYTES)
        return 0;

    server = data[0] & 1;
    accept = data[0] & 2;
    now = (uint32_t)data[1] << 8;
    data += CTRL_BYTES;
    size -= CTRL_BYTES;

    c = netchan_open(server);
    if (!c)
        return 0;

    /*
     * A server that never accepts drops every frame early, so most of the
     * parser would be unreachable. Accepting first is what opens it up.
     */
    if (server && accept)
        netchan_accept(c);

    /* An address the core copies and compares but never interprets. */
    memset(&from, 0, sizeof(from));
    from.len = 7;
    from.a[0] = 4;
    from.a[1] = 127;
    from.a[4] = 1;
    from.a[5] = 0x27;
    from.a[6] = 0x10;

    netchan_feed(c, data, size, &from);

    while (netchan_poll(c, &ev)) {
        if (ev.ch) {
            uint8_t msg[2048];

            while (netchan_chan_read(ev.ch, msg, sizeof(msg)) > 0)
                ;
        }
    }

    netchan_service(c, now);

    for (;;) {
        struct nc_addr to;
        size_t n = netchan_send_next(c, out, sizeof(out), &to);

        if (n == 0)
            break;
    }

    netchan_close(c);
    return 0;
}

#ifdef FUZZ_STANDALONE
#include <stdio.h>

/*
 * Replay driver, so a corpus can be run by any compiler with no fuzzing
 * runtime. This is what keeps a crashing input in the tree as a regression
 * test after the fuzzer has moved on.
 */
int
main(int argc, char **argv)
{
    uint8_t buf[65536];

    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t n;

        if (!f) {
            fprintf(stderr, "fuzz_netchan_feed: cannot open %s\n", argv[i]);
            return 1;
        }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("fuzz_netchan_feed: replayed %d input(s)\n", argc - 1);
    return 0;
}
#endif

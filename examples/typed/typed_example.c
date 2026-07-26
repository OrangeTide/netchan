/* typed_example.c : structured messages on a netchan channel via microser */

/*
 * netchan moves opaque bytes; microser decides what they mean. This shows the
 * two layers of that on one connection:
 *
 *   - The channel's content_type is a topic, in the MQTT sense. Here it is
 *     "chat", set when the sender opens the channel and read back by the
 *     receiver from the channel's OPEN. A program routes a channel by its
 *     topic before it looks at a single byte of payload.
 *
 *   - Within the topic, each datagram is <tag byte><microser body>. The
 *     dispatch block in chat.idl generates chat_encode_* to prefix the tag
 *     and chat_decode to read it back into a tagged union, so several message
 *     types share the one ordered channel.
 *
 * No socket and no event loop: the two connections are wired by a function
 * that hands one side's output straight to the other, so the message flow is
 * all that is left to read. A real program swaps that function for a socket.
 */

#include "netchan.h"
#include "chat.h"

#include <stdio.h>
#include <string.h>

#define TOPIC "chat"

static int failures;

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

/* Build a loopback nc_addr; in this demo it is only an opaque routing token. */
static struct nc_addr
make_addr(uint16_t port)
{
    struct nc_addr a;

    memset(&a, 0, sizeof(a));
    a.len = 7;
    a.a[0] = 4;
    a.a[1] = 127; a.a[2] = 0; a.a[3] = 0; a.a[4] = 1;
    a.a[5] = (uint8_t)(port >> 8);
    a.a[6] = (uint8_t)(port & 0xff);
    return a;
}

/* Move every queued datagram from one connection to the other. */
static void
pump(struct netchan_conn *from, struct netchan_conn *to,
     const struct nc_addr *from_addr)
{
    uint8_t buf[2048];
    struct nc_addr dst;
    size_t n;

    while ((n = netchan_send_next(from, buf, sizeof(buf), &dst)) != 0)
        netchan_feed(to, buf, n, from_addr);
}

static void
pump_both(struct netchan_conn *a, struct netchan_conn *b,
          const struct nc_addr *aa, const struct nc_addr *ba)
{
    int i;

    for (i = 0; i < 8; i++) {
        pump(a, b, aa);
        pump(b, a, ba);
    }
}

/* The receiver: route by topic, then dispatch on the tag byte. */
static void
handle(struct netchan_chan *ch, const uint8_t *buf, int len)
{
    struct chat_msg m;

    /* Route by topic first. A program with more than one channel decides
     * which decoder a datagram belongs to entirely from this string. */
    if (strcmp(netchan_chan_content_type(ch), TOPIC) != 0) {
        fprintf(stderr, "FAIL: datagram on an unexpected topic \"%s\"\n",
                netchan_chan_content_type(ch));
        failures++;
        return;
    }

    if (chat_decode(buf, len, &m) < 0) {
        fprintf(stderr, "FAIL: malformed message on \"%s\"\n", TOPIC);
        failures++;
        return;
    }

    switch (m.type) {
    case CHAT_JOIN:
        printf("  server: %.*s joined\n", m.u.join.name_len, m.u.join.name);
        break;
    case CHAT_SAY:
        printf("  server: <%.*s>\n", m.u.say.text_len, m.u.say.text);
        break;
    case CHAT_LEAVE:
        printf("  server: left, reason %u\n", m.u.leave.reason);
        break;
    default:
        /* A message type this build does not know. chat_decode has already
         * stepped over it; a real server would log and carry on. */
        printf("  server: (ignored an unknown message)\n");
        break;
    }
}

int
main(void)
{
    struct netchan_conn *client = netchan_open(0);
    struct netchan_conn *server = netchan_open(1);
    struct nc_addr caddr = make_addr(10000);
    struct nc_addr saddr = make_addr(20000);
    struct netchan_event ev;
    struct netchan_chan *tx, *rx = NULL;
    struct join j;
    struct say s;
    struct leave lv;
    uint8_t buf[512];
    int n, delivered = 0;

    CHECK(client && server, "netchan_open failed");
    if (!client || !server)
        return 1;

    /* Handshake. */
    netchan_connect(client, &saddr);
    pump(client, server, &caddr);
    netchan_accept(server);
    pump_both(client, server, &caddr, &saddr);
    while (netchan_poll(client, &ev)) {}
    while (netchan_poll(server, &ev)) {}

    /* Open the topic channel and let the OPEN reach the server. */
    tx = netchan_chan_open(client, NETCHAN_RELIABLE, NETCHAN_DIR_SEND, TOPIC);
    CHECK(tx != NULL, "chan_open failed");
    pump_both(client, server, &caddr, &saddr);
    while (netchan_poll(server, &ev))
        if (ev.type == NETCHAN_EV_CHAN_OPEN)
            rx = ev.ch;
    CHECK(rx != NULL, "server never saw the channel open");
    CHECK(rx && strcmp(netchan_chan_content_type(rx), TOPIC) == 0,
          "topic did not cross with the channel");

    /* Send three different message types on the one channel. */
    j.name = "alice"; j.name_len = 5;
    n = chat_encode_join(buf, sizeof(buf), &j);
    CHECK(n > 0 && netchan_chan_write(tx, buf, (size_t)n) == n, "write Join");

    s.text = "hello, netchan"; s.text_len = 14;
    n = chat_encode_say(buf, sizeof(buf), &s);
    CHECK(n > 0 && netchan_chan_write(tx, buf, (size_t)n) == n, "write Say");

    lv.reason = 0;
    n = chat_encode_leave(buf, sizeof(buf), &lv);
    CHECK(n > 0 && netchan_chan_write(tx, buf, (size_t)n) == n, "write Leave");

    /* Deliver, and drain what the server received. */
    pump_both(client, server, &caddr, &saddr);
    while (netchan_poll(server, &ev)) {
        if (ev.type == NETCHAN_EV_DATA && ev.ch) {
            while ((n = netchan_chan_read(ev.ch, buf, sizeof(buf))) > 0) {
                handle(ev.ch, buf, n);
                delivered++;
            }
        }
    }
    CHECK(delivered == 3, "server did not receive all three messages");

    netchan_close(client);
    netchan_close(server);

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nok: three message types over one topic channel\n");
    return 0;
}

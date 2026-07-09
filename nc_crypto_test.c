/* nc_crypto_test.c : run a netchan session over the encrypted decorator */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "netchan.h"
#include "nc_crypto.h"
#include <stdio.h>
#include <string.h>

static int tests_run, tests_passed;

#define TEST(name) do { tests_run++; printf("  %-46s ", name); fflush(stdout); } while (0)
#define PASS()     do { tests_passed++; printf("OK\n"); } while (0)
#define FAIL(msg)  do { printf("FAIL: %s\n", msg); return; } while (0)
#define CHECK(c, msg) do { if (!(c)) FAIL(msg); } while (0)

static struct nc_addr
make_addr(uint8_t last, uint16_t port)
{
    struct nc_addr a;
    memset(&a, 0, sizeof(a));
    a.len = 7;
    a.a[0] = 4; a.a[1] = 127; a.a[2] = 0; a.a[3] = 0; a.a[4] = last;
    a.a[5] = (port >> 8) & 0xff; a.a[6] = port & 0xff;
    return a;
}

/* Exchange HELLOs both ways until both sides hold session keys. */
static int
do_handshake(struct nc_crypto *cc, struct nc_crypto *sc)
{
    uint8_t hp[64], scratch[2048];
    for (int i = 0; i < 4 && !(nc_crypto_ready(cc) && nc_crypto_ready(sc)); i++) {
        size_t n = nc_crypto_handshake_packet(cc, hp, sizeof(hp));
        nc_crypto_open(sc, hp, n, scratch, sizeof(scratch));
        n = nc_crypto_handshake_packet(sc, hp, sizeof(hp));
        nc_crypto_open(cc, hp, n, scratch, sizeof(scratch));
    }
    return nc_crypto_ready(cc) && nc_crypto_ready(sc);
}

/* Drain netchan `from`, seal each datagram, open it on the far side, feed. */
static int
cpump(struct netchan_conn *from, struct nc_crypto *fc,
      struct netchan_conn *to, struct nc_crypto *tc,
      const struct nc_addr *from_addr)
{
    uint8_t buf[2048], sealed[2100], plain[2048];
    struct nc_addr dst = {0};
    int count = 0;
    for (;;) {
        size_t n = netchan_send_next(from, buf, sizeof(buf), &dst);
        if (n == 0) break;
        long sn = nc_crypto_seal(fc, buf, n, sealed, sizeof(sealed));
        if (sn < 0) break;
        long pn = nc_crypto_open(tc, sealed, (size_t)sn, plain, sizeof(plain));
        if (pn > 0)
            netchan_feed(to, plain, (size_t)pn, from_addr);
        count++;
    }
    return count;
}

static void
cboth(struct netchan_conn *cl, struct nc_crypto *cc,
      struct netchan_conn *sv, struct nc_crypto *sc,
      const struct nc_addr *caddr, const struct nc_addr *saddr)
{
    for (int i = 0; i < 12; i++) {
        int a = cpump(cl, cc, sv, sc, caddr);
        int b = cpump(sv, sc, cl, cc, saddr);
        if (a == 0 && b == 0) break;
    }
}

static void
test_encrypted_session(void)
{
    TEST("encrypted reliable session");

    uint8_t cseed[32], sseed[32];
    memset(cseed, 0x11, 32);
    memset(sseed, 0x22, 32);

    struct nc_crypto cc, sc;
    nc_crypto_init(&cc, 0, cseed, NULL);
    nc_crypto_init(&sc, 1, sseed, NULL);
    CHECK(do_handshake(&cc, &sc), "handshake did not complete");
    CHECK(memcmp(cc.tx_key, sc.rx_key, 32) == 0, "key mismatch c->s");
    CHECK(memcmp(sc.tx_key, cc.rx_key, 32) == 0, "key mismatch s->c");

    struct netchan_conn *cl = netchan_open(0);
    struct netchan_conn *sv = netchan_open(1);
    struct nc_addr caddr = make_addr(1, 10000);
    struct nc_addr saddr = make_addr(2, 20000);

    netchan_connect(cl, &saddr);
    cpump(cl, &cc, sv, &sc, &caddr);
    netchan_accept(sv);
    cboth(cl, &cc, sv, &sc, &caddr, &saddr);
    CHECK(netchan_state(cl) == NETCHAN_STATE_CONNECTED, "client not connected");

    struct netchan_event ev;
    while (netchan_poll(cl, &ev)) {}
    while (netchan_poll(sv, &ev)) {}

    struct netchan_chan *ch = netchan_chan_open(cl, NETCHAN_RELIABLE,
                                                NETCHAN_DIR_SEND, "state");
    CHECK(ch != NULL, "chan_open failed");
    cboth(cl, &cc, sv, &sc, &caddr, &saddr);
    while (netchan_poll(cl, &ev)) {}
    while (netchan_poll(sv, &ev)) {}

    const char *msg = "the quick brown fox, delivered under encryption";
    int wr = netchan_chan_write(ch, msg, strlen(msg));
    CHECK(wr == (int)strlen(msg), "write failed");
    cboth(cl, &cc, sv, &sc, &caddr, &saddr);

    struct netchan_chan *sch = NULL;
    while (netchan_poll(sv, &ev))
        if (ev.type == NETCHAN_EV_DATA && ev.ch) sch = ev.ch;
    CHECK(sch != NULL, "no data event on server");

    char rbuf[256];
    int rd = netchan_chan_read(sch, rbuf, sizeof(rbuf));
    CHECK(rd == (int)strlen(msg), "wrong read size");
    CHECK(memcmp(rbuf, msg, rd) == 0, "plaintext mismatch through cipher");

    netchan_close(cl);
    netchan_close(sv);
    PASS();
}

static void
test_tamper_rejected(void)
{
    TEST("tampered packet rejected");

    uint8_t s0[32], s1[32];
    memset(s0, 0xa1, 32); memset(s1, 0xb2, 32);
    struct nc_crypto a, b;
    nc_crypto_init(&a, 0, s0, NULL);
    nc_crypto_init(&b, 1, s1, NULL);
    CHECK(do_handshake(&a, &b), "handshake failed");

    const uint8_t plain[] = "position update";
    uint8_t sealed[128], out[128];
    long sn = nc_crypto_seal(&a, plain, sizeof(plain), sealed, sizeof(sealed));
    CHECK(sn > 0, "seal failed");

    sealed[NC_CRYPTO_OVERHEAD] ^= 0x01;   /* flip a ciphertext byte */
    CHECK(nc_crypto_open(&b, sealed, (size_t)sn, out, sizeof(out)) == -1,
          "forged packet was accepted");

    sealed[NC_CRYPTO_OVERHEAD] ^= 0x01;   /* restore -> must now verify */
    CHECK(nc_crypto_open(&b, sealed, (size_t)sn, out, sizeof(out)) == (long)sizeof(plain),
          "valid packet rejected after restore");
    PASS();
}

static void
test_replay_rejected(void)
{
    TEST("replayed packet rejected");

    uint8_t s0[32], s1[32];
    memset(s0, 0xc3, 32); memset(s1, 0xd4, 32);
    struct nc_crypto a, b;
    nc_crypto_init(&a, 0, s0, NULL);
    nc_crypto_init(&b, 1, s1, NULL);
    CHECK(do_handshake(&a, &b), "handshake failed");

    const uint8_t plain[] = "fire button pressed";
    uint8_t sealed[128], out[128];
    long sn = nc_crypto_seal(&a, plain, sizeof(plain), sealed, sizeof(sealed));
    CHECK(sn > 0, "seal failed");

    CHECK(nc_crypto_open(&b, sealed, (size_t)sn, out, sizeof(out)) > 0,
          "first delivery rejected");
    CHECK(nc_crypto_open(&b, sealed, (size_t)sn, out, sizeof(out)) == -1,
          "replayed packet was accepted");
    PASS();
}

static void
test_no_midsession_rekey(void)
{
    TEST("no mid-session re-key from a later HELLO");

    uint8_t s0[32], s1[32], s2[32];
    memset(s0, 0x01, 32); memset(s1, 0x02, 32); memset(s2, 0x03, 32);
    struct nc_crypto a, b, other;
    nc_crypto_init(&a, 0, s0, NULL);
    nc_crypto_init(&b, 1, s1, NULL);
    nc_crypto_init(&other, 0, s2, NULL);   /* a third party's ephemeral */
    CHECK(do_handshake(&a, &b), "handshake failed");

    uint8_t saved[32];
    memcpy(saved, b.rx_key, 32);

    /* a stray or injected HELLO after keys exist must be ignored, not rekey */
    uint8_t hp[64], out[64];
    size_t n = nc_crypto_handshake_packet(&other, hp, sizeof(hp));
    CHECK(nc_crypto_open(&b, hp, n, out, sizeof(out)) == 0, "late HELLO not consumed");
    CHECK(memcmp(b.rx_key, saved, 32) == 0, "keys changed after a later HELLO");
    PASS();
}

int
main(void)
{
    printf("nc_crypto tests:\n");
    test_encrypted_session();
    test_tamper_rejected();
    test_replay_rejected();
    test_no_midsession_rekey();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

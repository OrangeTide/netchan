/* proto_test.c : round-trip test for the generated game wire messages */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "proto.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) \
    do { if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while (0)

static void
test_player_input(void)
{
    struct player_input in = { .seq = 0xdeadbeef, .input = 0x62 };
    uint8_t buf[64];
    int n = player_input_encode(&in, buf, sizeof(buf));
    CHECK(n > 0);

    struct player_input out;
    memset(&out, 0, sizeof(out));
    int r = player_input_decode(&out, buf, n);
    CHECK(r > 0);
    CHECK(out.seq == in.seq);
    CHECK(out.input == in.input);
}

static void
test_welcome(void)
{
    struct welcome in = { .your_player = 2, .seed = 0xa5f0 };
    uint8_t buf[64];
    int n = welcome_encode(&in, buf, sizeof(buf));
    CHECK(n > 0);

    struct welcome out;
    memset(&out, 0, sizeof(out));
    int r = welcome_decode(&out, buf, n);
    CHECK(r > 0);
    CHECK(out.your_player == in.your_player);
    CHECK(out.seed == in.seed);
}

static void
test_snapshot(void)
{
    uint8_t state[224];
    for (size_t i = 0; i < sizeof(state); i++)
        state[i] = (uint8_t)(i * 7 + 1);

    struct snapshot in = { .tick = 4096, .state = state,
                           .state_len = (uint16_t)sizeof(state) };
    uint8_t buf[512];
    int n = snapshot_encode(&in, buf, sizeof(buf));
    CHECK(n > 0);

    struct snapshot out;
    memset(&out, 0, sizeof(out));
    int r = snapshot_decode(&out, buf, n);
    CHECK(r > 0);
    CHECK(out.tick == in.tick);
    CHECK(out.state_len == in.state_len);
    CHECK(memcmp(out.state, state, sizeof(state)) == 0);  /* zero-copy bytes */
}

int
main(void)
{
    test_player_input();
    test_welcome();
    test_snapshot();
    if (fails == 0)
        printf("proto round-trip OK (player_input, welcome, snapshot)\n");
    return fails ? 1 : 0;
}

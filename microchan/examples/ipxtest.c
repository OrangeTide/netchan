/* ipxtest.c : DOS IPX reliability test for microchan (run in DOSBox) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * Two instances, connected through DOSBox's IPX tunnel (and an ipxrelay):
 *   ipxtest s      server: counts reliable messages, reports PASS at PINGS
 *   ipxtest        client: discovers the server with a broadcast SYN, then
 *                  streams PINGS reliable messages
 * The loop is rate-limited to one BIOS tick (~18 Hz), the way a game loop
 * would be, and drains a bounded number of datagrams per tick.
 */

#include "microchan.h"
#include "mc_ipx.h"
#include <dos.h>
#include <i86.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>

#define GAME_SOCKET 0x6000
#define PINGS       40
#define TIMEOUT_MS  10000UL
#define MAX_DRAIN   64          /* cap datagrams handled per tick */

/* BIOS tick counter at 0040:006Ch, ~18.2 Hz (~55 ms). */
static uint32_t
ticks(void)
{
    uint32_t __far *t = (uint32_t __far *)MK_FP(0x0040, 0x006C);
    return *t;
}

static uint32_t
now_ms(void)
{
    return ticks() * 55u;
}

int
main(int argc, char **argv)
{
    struct mc_ipx x;
    struct microchan *c;
    struct mc_chan *rel, *unr;
    struct mc_addr from, to;
    uint8_t buf[MC_MTU];
    char msg[32], rb[64];
    int is_server = (argc > 1 && (argv[1][0] == 's' || argv[1][0] == 'S'));
    int connected = 0, sent = 0, recvd = 0, n, g;
    uint32_t start, now, lasttick;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("microchan IPX test - %s\n", is_server ? "SERVER" : "CLIENT");

    if (!mc_ipx_available()) {
        printf("FAIL: no IPX driver (is ipx=true and ipxnet connected?)\n");
        return 1;
    }
    if (mc_ipx_open(&x, GAME_SOCKET) != 0) {
        printf("FAIL: mc_ipx_open\n");
        return 1;
    }
    printf("node %02X:%02X:%02X:%02X:%02X:%02X sock %04X\n",
           x.node[0], x.node[1], x.node[2], x.node[3], x.node[4], x.node[5],
           GAME_SOCKET);

    c = mc_open(is_server ? 1 : 0);
    rel = mc_chan_open(c, MC_RELIABLE);
    unr = mc_chan_open(c, MC_UNRELIABLE);
    (void)unr;

    if (!is_server) {
        mc_ipx_broadcast(&x, &to);
        mc_connect(c, &to);
        printf("connecting (broadcast SYN)...\n");
    } else {
        printf("waiting for a client...\n");
    }

    start = now_ms();
    lasttick = ticks();
    for (;;) {
        struct mc_event ev;
        now = now_ms();
        if (now - start > TIMEOUT_MS)
            break;

        mc_service(c, now);

        for (g = 0; g < MAX_DRAIN; g++) {
            if ((n = mc_ipx_recv(&x, buf, sizeof(buf), &from)) <= 0)
                break;
            mc_feed(c, buf, (size_t)n, &from);
        }

        while (mc_poll(c, &ev)) {
            if (ev.type == MC_EV_CONNECTED) {
                connected = 1;
                printf("connected\n");
            } else if (ev.type == MC_EV_DISCONNECTED) {
                printf("disconnected\n");
            }
        }

        if (is_server) {
            while ((n = mc_read(rel, rb, sizeof(rb))) > 0) {
                if (++recvd >= PINGS)
                    break;
            }
            if (recvd >= PINGS)
                break;
        } else if (connected) {
            while (sent < PINGS) {
                int ml = sprintf(msg, "PING %d", sent);
                if (mc_write(rel, msg, (size_t)ml) == MC_ERR_AGAIN)
                    break;
                sent++;
            }
        }

        for (g = 0; g < MAX_DRAIN; g++) {
            if ((n = (int)mc_send_next(c, buf, sizeof(buf), &to)) <= 0)
                break;
            mc_ipx_send(&x, buf, (size_t)n, &to);
        }

        if (kbhit()) {
            getch();
            break;
        }

        while (ticks() == lasttick) {
            if (now_ms() - start > TIMEOUT_MS)
                break;
        }
        lasttick = ticks();
    }

    if (is_server)
        printf("%s: %d/%d reliable messages received\n",
               recvd >= PINGS ? "PASS" : "FAIL", recvd, PINGS);
    else
        printf("client done: %d/%d queued\n", sent, PINGS);

    mc_close(c);
    mc_ipx_close(&x);
    return 0;
}

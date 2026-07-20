/* mcdemo.c : phase 1 link/smoke test for the microchan core */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#include "microchan.h"
#include <stdio.h>

int
main(void)
{
    struct microchan *c;
    struct mc_chan *rel, *unrel;

    c = mc_open(0);
    if (!c) {
        printf("mc_open failed\n");
        return 1;
    }

    rel = mc_chan_open(c, MC_RELIABLE);
    unrel = mc_chan_open(c, MC_UNRELIABLE);

    printf("microchan core OK: state=%d reliable_chan=%d unreliable_chan=%d\n",
           mc_state(c), mc_chan_id(rel), mc_chan_id(unrel));
    printf("config: MTU=%d WINDOW=%d MAX_CHAN=%d MAX_CONN=%d\n",
           MC_MTU, MC_WINDOW, MC_MAX_CHAN, MC_MAX_CONN);

    mc_close(c);
    return 0;
}

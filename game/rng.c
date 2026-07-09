/* rng.c : tiny X-ABC 8-bit PRNG (see rng.h) */

#include "rng.h"

static unsigned char rng_x, rng_a, rng_b, rng_c;

void
rng_seed(unsigned char s1, unsigned char s2, unsigned char s3)
{
    rng_a ^= s1;
    rng_b ^= s2;
    rng_c ^= s3;
    rng_next();
}

unsigned char
rng_next(void)
{
    rng_x++;
    rng_a = (rng_a ^ rng_c) ^ rng_x;
    rng_b = rng_b + rng_a;
    rng_c = (unsigned char)(rng_c + ((rng_b >> 1) | (rng_b << 7))) ^ rng_a;
    return rng_c;
}

unsigned
rng_range(unsigned n)
{
    unsigned r, lim;

    if (n <= 1)
        return 0;
    if (n <= 256) {
        lim = 256u - (256u % n);    // largest multiple of n that fits a byte
        do {
            r = rng_next();
        } while (r >= lim);
        return r % n;
    } else {
        unsigned long big, l2;
        unsigned hi, lo;

        l2 = 65536uL - (65536uL % n);
        do {
            hi = rng_next();        // sequence the two draws explicitly
            lo = rng_next();
            big = ((unsigned long)hi << 8) | lo;
        } while (big >= l2);
        return (unsigned)(big % n);
    }
}

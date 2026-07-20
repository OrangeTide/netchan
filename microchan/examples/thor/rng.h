/* rng.h : tiny X-ABC 8-bit PRNG, shared by the engine and host tools */

#ifndef RNG_H
#define RNG_H

/* Mix three seed bytes into the generator state. */
void rng_seed(unsigned char s1, unsigned char s2, unsigned char s3);

/* Return the next pseudo-random byte. */
unsigned char rng_next(void);

/* Return a value in [0, n), rejection-sampled to avoid modulo bias. n <= 1
 * yields 0. Pulls two bytes when n > 256. */
unsigned rng_range(unsigned n);

#endif /* RNG_H */

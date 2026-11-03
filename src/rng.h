#ifndef RNG_H
#define RNG_H

#include <stdint.h>

// WARNING: this is a weak PRNG (xorshift64, seeded from RDTSC jitter at
// boot). It is fine for non-security-critical randomization like MAC
// addresses. It must NOT be used for cryptographic key material, Tor
// circuit IDs, or anything where an attacker predicting the output
// would matter. A real CSPRNG (ideally seeded from RDRAND/RDSEED where
// available) is a separate, later piece of work.

void rng_init(void);
uint64_t rng_next(void);
void rng_fill(uint8_t *buf, uint32_t len);

#endif

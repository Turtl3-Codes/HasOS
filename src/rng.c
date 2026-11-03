#include "rng.h"

static uint64_t g_state;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void rng_init(void) {
    g_state = rdtsc();
    if (g_state == 0) {
        g_state = 0x9E3779B97F4A7C15ULL;
    }
}

uint64_t rng_next(void) {
    g_state ^= g_state << 13;
    g_state ^= g_state >> 7;
    g_state ^= g_state << 17;
    return g_state;
}

void rng_fill(uint8_t *buf, uint32_t len) {
    uint32_t i = 0;
    while (i < len) {
        uint64_t r = rng_next();
        for (int b = 0; b < 8 && i < len; b++, i++) {
            buf[i] = (uint8_t)(r >> (b * 8));
        }
    }
}

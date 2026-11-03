#include "power.h"
#include "serial.h"
#include <stdint.h>

extern void pmm_secure_wipe(void);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void hasos_shutdown(void) {
    serial_print("Wiping RAM before shutdown...\n");
    pmm_secure_wipe();
    outw(0x604, 0x2000);
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void hasos_restart(void) {
    serial_print("Wiping RAM before reboot...\n");
    pmm_secure_wipe();
    for (int spins = 0; spins < 100000; spins++) {
        if (!(inb(0x64) & 0x02)) {
            break;
        }
    }
    outb(0x64, 0xFE);
    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

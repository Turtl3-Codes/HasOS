#ifndef NET_CONFIG_H
#define NET_CONFIG_H
#include <stdint.h>

// Static IP config matching QEMU user-mode networking defaults.
#define NET_OUR_IP  0x0A00020FUL   // 10.0.2.15
#define NET_GATEWAY 0x0A000202UL   // 10.0.2.2

#endif

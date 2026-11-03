#ifndef ICMP_H
#define ICMP_H
#include <stdint.h>

void icmp_handle(const uint8_t *pkt, uint16_t len, uint32_t src_ip, const uint8_t src_mac[6]);

#endif

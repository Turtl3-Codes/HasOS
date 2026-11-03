#ifndef ARP_H
#define ARP_H
#include <stdint.h>

void arp_handle_frame(const uint8_t *pkt, uint16_t len, const uint8_t src_mac[6]);

#endif

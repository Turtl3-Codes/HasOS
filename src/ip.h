#ifndef IP_H
#define IP_H
#include <stdint.h>

void ip_handle_packet(const uint8_t *pkt, uint16_t len, const uint8_t src_mac[6]);
int  ip_send_reply(uint32_t dst_ip, const uint8_t dst_mac[6], uint8_t protocol, const uint8_t *payload, uint16_t len);
uint16_t ip_checksum(const void *data, uint32_t len);

#endif

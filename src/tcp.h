#ifndef TCP_H
#define TCP_H
#include <stdint.h>
void tcp_init(void);
void tcp_handle_packet(const uint8_t *pkt, uint16_t len, uint32_t src_ip, const uint8_t src_mac[6]);
void tcp_listen(uint16_t port, const char *response, uint16_t response_len);
#endif

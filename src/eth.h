#ifndef ETH_H
#define ETH_H
#include <stdint.h>

void eth_init(void);
void eth_randomize_mac(void);
void eth_handle_frame(const uint8_t *frame, uint16_t len);
int  eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const uint8_t *payload, uint16_t payload_len);
void eth_get_our_mac(uint8_t mac_out[6]);

#endif

#include "arp.h"
#include "eth.h"
#include "net_config.h"
#include <stdint.h>

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

void arp_handle_frame(const uint8_t *pkt, uint16_t len, const uint8_t src_mac[6]) {
    (void)src_mac;
    if (len < 28) return;

    uint16_t hw_type = ((uint16_t)pkt[0] << 8) | pkt[1];
    uint16_t proto_type = ((uint16_t)pkt[2] << 8) | pkt[3];
    uint16_t opcode = ((uint16_t)pkt[6] << 8) | pkt[7];
    if (hw_type != 1 || proto_type != 0x0800) return;

    uint32_t target_ip = rd32(pkt + 24);
    extern void serial_print(const char *str);
    serial_print("arp: request received\n");
    if (opcode != 1 || target_ip != NET_OUR_IP) {
        serial_print("arp: rejected (opcode or target_ip mismatch)\n");
        return;
    }

    uint8_t sender_mac[6];
    for (int i = 0; i < 6; i++) sender_mac[i] = pkt[8 + i];
    uint32_t sender_ip = rd32(pkt + 14);

    uint8_t our_mac[6];
    eth_get_our_mac(our_mac);

    uint8_t reply[28];
    reply[0] = 0; reply[1] = 1;       // hw_type = ethernet
    reply[2] = 0x08; reply[3] = 0x00; // proto_type = ipv4
    reply[4] = 6;                     // hw addr len
    reply[5] = 4;                     // proto addr len
    reply[6] = 0; reply[7] = 2;       // opcode = reply
    for (int i = 0; i < 6; i++) reply[8 + i] = our_mac[i];
    wr32(reply + 14, NET_OUR_IP);
    for (int i = 0; i < 6; i++) reply[18 + i] = sender_mac[i];
    wr32(reply + 24, sender_ip);

    eth_send(sender_mac, 0x0806, reply, 28);
}

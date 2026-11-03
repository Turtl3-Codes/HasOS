#include "icmp.h"
#include "ip.h"
#include <stdint.h>

void icmp_handle(const uint8_t *pkt, uint16_t len, uint32_t src_ip, const uint8_t src_mac[6]) {
    static uint8_t reply[1024];
    if (len < 8 || len > sizeof(reply)) return;
    if (pkt[0] != 8) return; // only handle echo request (type 8)

    for (uint16_t i = 0; i < len; i++) reply[i] = pkt[i];
    reply[0] = 0; // echo reply
    reply[1] = 0;
    reply[2] = 0; reply[3] = 0; // checksum, filled below

    uint16_t csum = ip_checksum(reply, len);
    reply[2] = (uint8_t)(csum >> 8);
    reply[3] = (uint8_t)(csum & 0xFF);

    ip_send_reply(src_ip, src_mac, 1, reply, len);
}

#include "ip.h"
#include "eth.h"
#include "net_config.h"
#include <stdint.h>

extern void icmp_handle(const uint8_t *pkt, uint16_t len, uint32_t src_ip, const uint8_t src_mac[6]);
extern void tcp_handle_packet(const uint8_t *pkt, uint16_t len, uint32_t src_ip, const uint8_t src_mac[6]);

static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len == 1) sum += ((uint32_t)p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

void ip_handle_packet(const uint8_t *pkt, uint16_t len, const uint8_t src_mac[6]) {
    if (len < 20) return;
    uint8_t ihl = (pkt[0] & 0x0F) * 4;
    if (ihl < 20 || len < ihl) return;

    uint8_t protocol = pkt[9];
    uint32_t dst_ip = rd32(pkt + 16);
    uint32_t src_ip = rd32(pkt + 12);
    if (dst_ip != NET_OUR_IP) return;

    uint16_t total_len = ((uint16_t)pkt[2] << 8) | pkt[3];
    if (total_len > len) total_len = len;

    if (protocol == 1) { // ICMP
        icmp_handle(pkt + ihl, (uint16_t)(total_len - ihl), src_ip, src_mac);
    } else if (protocol == 6) { // TCP
        tcp_handle_packet(pkt + ihl, (uint16_t)(total_len - ihl), src_ip, src_mac);
    }
}

int ip_send_reply(uint32_t dst_ip, const uint8_t dst_mac[6], uint8_t protocol, const uint8_t *payload, uint16_t len) {
    static uint8_t buf[1500];
    if ((uint32_t)len + 20 > sizeof(buf)) return 0;

    buf[0] = 0x45; // version 4, ihl 5
    buf[1] = 0;
    uint16_t total_len = (uint16_t)(20 + len);
    buf[2] = (uint8_t)(total_len >> 8); buf[3] = (uint8_t)(total_len & 0xFF);
    buf[4] = 0; buf[5] = 0;   // id
    buf[6] = 0; buf[7] = 0;   // flags/frag
    buf[8] = 64;              // ttl
    buf[9] = protocol;
    buf[10] = 0; buf[11] = 0; // checksum, filled below
    wr32(buf + 12, NET_OUR_IP);
    wr32(buf + 16, dst_ip);

    uint16_t csum = ip_checksum(buf, 20);
    buf[10] = (uint8_t)(csum >> 8);
    buf[11] = (uint8_t)(csum & 0xFF);

    for (uint16_t i = 0; i < len; i++) buf[20 + i] = payload[i];

    return eth_send(dst_mac, 0x0800, buf, total_len);
}

#include "eth.h"
#include "virtio.h"
#include "rng.h"
#include <stdint.h>

extern void serial_print(const char *str);
extern void serial_print_hex(uint64_t val);
extern void arp_handle_frame(const uint8_t *pkt, uint16_t len, const uint8_t src_mac[6]);
extern void ip_handle_packet(const uint8_t *pkt, uint16_t len, const uint8_t src_mac[6]);

static uint8_t g_our_mac[6];
static uint8_t g_tx_buf[1518];

// Generates a random, locally-administered, unicast MAC address. Setting
// the locally-administered bit (and clearing the multicast bit) on the
// first byte marks this as not a real IEEE-assigned hardware address,
// per the standard convention (same one Linux/Tails use for randomized
// MACs), so it is clearly distinguishable from a genuine burned-in NIC
// address to anything inspecting it.
void eth_randomize_mac(void) {
    rng_fill(g_our_mac, 6);
    g_our_mac[0] &= 0xFE; // clear multicast bit
    g_our_mac[0] |= 0x02; // set locally-administered bit
}

void eth_init(void) {
    rng_init();
    eth_randomize_mac();
}

void eth_get_our_mac(uint8_t mac_out[6]) {
    for (int i = 0; i < 6; i++) mac_out[i] = g_our_mac[i];
}

int eth_send(const uint8_t dst_mac[6], uint16_t ethertype, const uint8_t *payload, uint16_t payload_len) {
    if (payload_len > sizeof(g_tx_buf) - 14) return 0;

    for (int i = 0; i < 6; i++) g_tx_buf[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) g_tx_buf[6 + i] = g_our_mac[i];
    g_tx_buf[12] = (uint8_t)(ethertype >> 8);
    g_tx_buf[13] = (uint8_t)(ethertype & 0xFF);

    for (uint16_t i = 0; i < payload_len; i++) g_tx_buf[14 + i] = payload[i];

    return virtio_net_send_frame(g_tx_buf, (uint16_t)(14 + payload_len));
}

void eth_handle_frame(const uint8_t *frame, uint16_t len) {
    serial_print("eth: frame received, len=");
    serial_print_hex(len);
    serial_print("\n");
    if (len < 14) return;

    uint8_t src_mac[6];
    for (int i = 0; i < 6; i++) src_mac[i] = frame[6 + i];
    uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];

    const uint8_t *payload = frame + 14;
    uint16_t payload_len = len - 14;

    if (ethertype == 0x0806) {
        arp_handle_frame(payload, payload_len, src_mac);
    } else if (ethertype == 0x0800) {
        ip_handle_packet(payload, payload_len, src_mac);
    }
}

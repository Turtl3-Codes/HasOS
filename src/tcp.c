#include "tcp.h"
#include "ip.h"
#include "net_config.h"
#include "timer.h"
#include <stdint.h>
extern void serial_print(const char *str);

// Minimal single-connection TCP: one listening port, one peer at a
// time, no retransmission, no window scaling. Enough to complete a
// real handshake and serve a canned HTTP response to curl/a browser.
// Known limitations: a lost segment hangs the connection forever (no
// timeout/retransmit); a second peer connecting while one is already
// active is silently ignored until the first closes.

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_LAST_ACK
} tcp_state_t;

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_PSH 0x08

static tcp_state_t g_state = TCP_CLOSED;
static uint16_t g_listen_port = 0;
static const char *g_response = 0;
static uint16_t g_response_len = 0;

static uint32_t g_remote_ip;
static uint8_t g_remote_mac[6];
static uint16_t g_remote_port;
static uint32_t g_snd_nxt;
static uint32_t g_rcv_nxt;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v & 0xFF);
}
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int tcp_send_segment(uint32_t dst_ip, const uint8_t dst_mac[6],
                             uint16_t src_port, uint16_t dst_port,
                             uint32_t seq, uint32_t ack, uint8_t flags,
                             const uint8_t *payload, uint16_t payload_len) {
    static uint8_t segbuf[1460];
    static uint8_t csumbuf[12 + 1460];
    if ((uint32_t)20 + payload_len > sizeof(segbuf)) return 0;

    wr16(segbuf + 0, src_port);
    wr16(segbuf + 2, dst_port);
    wr32(segbuf + 4, seq);
    wr32(segbuf + 8, ack);
    segbuf[12] = 5 << 4;           // data offset = 5 words (20 bytes), no options
    segbuf[13] = flags;
    wr16(segbuf + 14, 8192);       // window, fixed
    segbuf[16] = 0; segbuf[17] = 0; // checksum, filled below
    wr16(segbuf + 18, 0);          // urgent pointer
    for (uint16_t i = 0; i < payload_len; i++) segbuf[20 + i] = payload[i];
    uint16_t seg_len = (uint16_t)(20 + payload_len);

    wr32(csumbuf + 0, NET_OUR_IP);
    wr32(csumbuf + 4, dst_ip);
    csumbuf[8] = 0;
    csumbuf[9] = 6; // TCP
    wr16(csumbuf + 10, seg_len);
    for (uint16_t i = 0; i < seg_len; i++) csumbuf[12 + i] = segbuf[i];
    uint16_t csum = ip_checksum(csumbuf, (uint32_t)(12 + seg_len));
    segbuf[16] = (uint8_t)(csum >> 8);
    segbuf[17] = (uint8_t)(csum & 0xFF);

    return ip_send_reply(dst_ip, dst_mac, 6, segbuf, seg_len);
}

void tcp_init(void) {
    g_state = TCP_CLOSED;
}

void tcp_listen(uint16_t port, const char *response, uint16_t response_len) {
    g_listen_port = port;
    g_response = response;
    g_response_len = response_len;
    g_state = TCP_LISTEN;
}

void tcp_handle_packet(const uint8_t *pkt, uint16_t len, uint32_t src_ip, const uint8_t src_mac[6]) {
    if (len < 20) return;
    uint16_t src_port = rd16(pkt + 0);
    uint16_t dst_port = rd16(pkt + 2);
    uint32_t seq = rd32(pkt + 4);
    uint8_t data_offset = (uint8_t)((pkt[12] >> 4) * 4);
    uint8_t flags = pkt[13];
    if (data_offset < 20 || data_offset > len) return;
    const uint8_t *payload = pkt + data_offset;
    (void)payload;
    uint16_t payload_len = (uint16_t)(len - data_offset);

    if (dst_port != g_listen_port) return;

    switch (g_state) {
    case TCP_LISTEN:
        if (flags & TCP_FLAG_SYN) {
            g_remote_ip = src_ip;
            for (int i = 0; i < 6; i++) g_remote_mac[i] = src_mac[i];
            g_remote_port = src_port;
            g_rcv_nxt = seq + 1;
            g_snd_nxt = 0x1000 + (uint32_t)timer_get_ticks();
            serial_print("tcp: SYN received, sending SYN-ACK\n");
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_SYN | TCP_FLAG_ACK, 0, 0);
            g_snd_nxt++;
            g_state = TCP_SYN_RECEIVED;
        }
        break;

    case TCP_SYN_RECEIVED:
        if (src_ip != g_remote_ip || src_port != g_remote_port) break;
        if (flags & TCP_FLAG_ACK) {
            serial_print("tcp: connection established\n");
            g_state = TCP_ESTABLISHED;
        }
        break;

    case TCP_ESTABLISHED:
        if (src_ip != g_remote_ip || src_port != g_remote_port) break;
        if (flags & TCP_FLAG_FIN) {
            g_rcv_nxt = seq + 1;
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_ACK, 0, 0);
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            g_snd_nxt++;
            g_state = TCP_LAST_ACK;
            break;
        }
        if (payload_len > 0) {
            g_rcv_nxt = seq + payload_len;
            serial_print("tcp: request received, sending response\n");
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_ACK, 0, 0);
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_PSH | TCP_FLAG_ACK,
                              (const uint8_t *)g_response, g_response_len);
            g_snd_nxt += g_response_len;
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
            g_snd_nxt++;
            g_state = TCP_FIN_WAIT_1;
        }
        break;

    case TCP_FIN_WAIT_1:
        if (src_ip != g_remote_ip || src_port != g_remote_port) break;
        if (flags & TCP_FLAG_FIN) {
            g_rcv_nxt = seq + 1;
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_ACK, 0, 0);
            serial_print("tcp: connection closed\n");
            g_state = TCP_LISTEN;
        } else if (flags & TCP_FLAG_ACK) {
            g_state = TCP_FIN_WAIT_2;
        }
        break;

    case TCP_FIN_WAIT_2:
        if (src_ip != g_remote_ip || src_port != g_remote_port) break;
        if (flags & TCP_FLAG_FIN) {
            g_rcv_nxt = seq + 1;
            tcp_send_segment(src_ip, src_mac, g_listen_port, src_port,
                              g_snd_nxt, g_rcv_nxt, TCP_FLAG_ACK, 0, 0);
            serial_print("tcp: connection closed\n");
            g_state = TCP_LISTEN;
        }
        break;

    case TCP_LAST_ACK:
        if (src_ip != g_remote_ip || src_port != g_remote_port) break;
        if (flags & TCP_FLAG_ACK) {
            serial_print("tcp: connection closed\n");
            g_state = TCP_LISTEN;
        }
        break;

    default:
        break;
    }
}

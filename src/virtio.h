#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>

#define VIRTIO_VENDOR_ID          0x1AF4
#define VIRTIO_NET_DEVICE_LEGACY  0x1000
#define VIRTIO_NET_DEVICE_MODERN  0x1041

#define PCI_CAP_ID_VNDR 0x09

#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG    3
#define VIRTIO_PCI_CAP_DEVICE_CFG 4

#define VIRTIO_STATUS_ACKNOWLEDGE 1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      0x80

#define VIRTIO_NET_F_MAC_BIT     5   // bit within the low 32 feature bits
#define VIRTIO_F_VERSION_1_BIT   0   // bit within the high 32 feature bits

#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#define VIRTIO_NET_QUEUE_SIZE   16
#define VIRTIO_NET_RX_BUF_SIZE  2048
#define VIRTIO_NET_RXQ 0
#define VIRTIO_NET_TXQ 1

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) virtq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_NET_QUEUE_SIZE];
} __attribute__((packed)) virtq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) virtq_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[VIRTIO_NET_QUEUE_SIZE];
} __attribute__((packed)) virtq_used_t;

typedef struct {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers; // present whenever VIRTIO_F_VERSION_1 is negotiated,
                          // regardless of whether MRG_RXBUF itself was --
                          // QEMU's modern virtio-net-pci sends this 12-byte
                          // layout on RX either way. Unused/zeroed on TX.
} __attribute__((packed)) virtio_net_hdr_t;

typedef struct {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t  device_status;
    uint8_t  config_generation;
    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
} __attribute__((packed)) virtio_pci_common_cfg_t;

void virtio_net_init(void);
void virtio_net_handle_irq(void);
int  virtio_net_send_frame(const uint8_t *frame, uint16_t len);
int  virtio_net_is_ready(void);
void virtio_net_get_mac(uint8_t mac_out[6]);

// implemented in netstack.c
extern void net_irq_dispatch(uint8_t irq);

#endif

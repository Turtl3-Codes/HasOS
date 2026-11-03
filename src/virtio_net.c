#include "virtio.h"
#include "pci.h"
#include "vmm.h"
#include "pmm.h"
#include "pic.h"
#include "idt.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *str);
void serial_print_hex(uint64_t val) {
    char buf[19] = "0x0000000000000000";
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        buf[17 - i] = digits[val & 0xF];
        val >>= 4;
    }
    serial_print(buf);
}

// Interrupt stubs added to isr_stubs.asm for the common legacy PCI IRQ lines.
extern void irq5(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);

#define VIRTIO_COMMON_VA 0xFFFFB00000000000UL
#define VIRTIO_NOTIFY_VA 0xFFFFB00001000000UL
#define VIRTIO_ISR_VA    0xFFFFB00002000000UL
#define VIRTIO_DEVICE_VA 0xFFFFB00003000000UL

typedef struct {
    uint64_t desc_phys, avail_phys, used_phys;
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;
    uint16_t size;
    uint16_t notify_off;
    uint16_t last_used_idx;
} virtq_t;

static uint8_t g_bus, g_dev, g_func;
static int g_found = 0;
static int g_ready = 0;
static uint8_t g_irq_line = 0xFF;
static uint8_t g_mac[6];

static volatile virtio_pci_common_cfg_t *g_common = NULL;
static volatile uint8_t *g_isr = NULL;
static volatile uint8_t *g_device_cfg = NULL;
static volatile uint8_t *g_notify_base = NULL;
static uint32_t g_notify_off_multiplier = 0;

static virtq_t g_rxq, g_txq;
static uint64_t g_rx_buf_phys[VIRTIO_NET_QUEUE_SIZE];
static uint64_t g_tx_hdr_phys;

static inline uint64_t phys_to_virt(uint64_t phys) {
    extern uint64_t pmm_get_hhdm_offset(void);
    return phys + pmm_get_hhdm_offset();
}

static void vnet_memset(void *dst, uint8_t val, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) d[i] = val;
}

static void vnet_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

static uint64_t read_bar_address(uint8_t bus, uint8_t dev, uint8_t func, uint8_t bar_index) {
    uint32_t low = pci_config_read32(bus, dev, func, 0x10 + bar_index * 4);
    if (low & 0x1) return 0; // I/O space bar, not supported here
    uint8_t type = (low >> 1) & 0x3;
    uint64_t addr = low & ~0xFUL;
    if (type == 0x2) {
        uint32_t high = pci_config_read32(bus, dev, func, 0x10 + (bar_index + 1) * 4);
        addr |= ((uint64_t)high) << 32;
    }
    return addr;
}

static void *map_bar_region(uint64_t bar_phys_base, uint32_t cap_offset, uint32_t cap_length, uint64_t va_window) {
    uint64_t phys_start = bar_phys_base + cap_offset;
    uint64_t page_phys_start = phys_start & ~0xFFFULL;
    uint64_t page_offset = phys_start - page_phys_start;
    uint32_t total_span = (uint32_t)(page_offset + cap_length);
    uint32_t pages = (total_span + 4095) / 4096;
    if (pages == 0) pages = 1;

    uint64_t *pml4 = vmm_get_kernel_directory();
    for (uint32_t i = 0; i < pages; i++) {
        vmm_map_page(pml4, va_window + (uint64_t)i * 4096,
                     page_phys_start + (uint64_t)i * 4096,
                     PAGE_PRESENT | PAGE_WRITE);
    }
    return (void *)(va_window + page_offset);
}

static void vnet_pci_cb(const pci_device_t *dev) {
    if (g_found) return;
    if (dev->vendor_id != VIRTIO_VENDOR_ID) return;
    if (dev->device_id == VIRTIO_NET_DEVICE_LEGACY || dev->device_id == VIRTIO_NET_DEVICE_MODERN) {
        g_found = 1;
        g_bus = dev->bus;
        g_dev = dev->device;
        g_func = dev->function;
    }
}

static int parse_capabilities(void) {
    uint32_t status_dword = pci_config_read32(g_bus, g_dev, g_func, 0x04);
    if (!(status_dword & (1UL << 20))) { // status bit4 (capabilities list) = dword bit 16+4
        serial_print("virtio-net: device has no capability list\n");
        return 0;
    }

    uint32_t cap_ptr_dword = pci_config_read32(g_bus, g_dev, g_func, 0x34);
    uint8_t cap_ptr = (uint8_t)(cap_ptr_dword & 0xFC);

    int have_common = 0, have_notify = 0, have_isr = 0, have_device = 0;

    while (cap_ptr != 0) {
        uint32_t d0 = pci_config_read32(g_bus, g_dev, g_func, cap_ptr);
        uint8_t cap_id   = (uint8_t)(d0 & 0xFF);
        uint8_t next_ptr = (uint8_t)((d0 >> 8) & 0xFF);
        uint8_t cfg_type = (uint8_t)((d0 >> 24) & 0xFF);

        if (cap_id == PCI_CAP_ID_VNDR) {
            uint32_t d1 = pci_config_read32(g_bus, g_dev, g_func, cap_ptr + 4);
            uint8_t bar = (uint8_t)(d1 & 0xFF);
            uint32_t offset = pci_config_read32(g_bus, g_dev, g_func, cap_ptr + 8);
            uint32_t length = pci_config_read32(g_bus, g_dev, g_func, cap_ptr + 12);
            uint64_t bar_phys = read_bar_address(g_bus, g_dev, g_func, bar);

            if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG && !have_common) {
                g_common = (volatile virtio_pci_common_cfg_t *)map_bar_region(bar_phys, offset, length, VIRTIO_COMMON_VA);
                have_common = 1;
            } else if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG && !have_notify) {
                g_notify_off_multiplier = pci_config_read32(g_bus, g_dev, g_func, cap_ptr + 16);
                g_notify_base = (volatile uint8_t *)map_bar_region(bar_phys, offset, length, VIRTIO_NOTIFY_VA);
                have_notify = 1;
            } else if (cfg_type == VIRTIO_PCI_CAP_ISR_CFG && !have_isr) {
                g_isr = (volatile uint8_t *)map_bar_region(bar_phys, offset, length, VIRTIO_ISR_VA);
                have_isr = 1;
            } else if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG && !have_device) {
                g_device_cfg = (volatile uint8_t *)map_bar_region(bar_phys, offset, length, VIRTIO_DEVICE_VA);
                have_device = 1;
            }
        }

        cap_ptr = next_ptr;
    }

    if (!have_common || !have_notify || !have_isr) {
        serial_print("virtio-net: missing required capability region\n");
        return 0;
    }
    return 1;
}

static void notify_queue(virtq_t *q) {
    volatile uint16_t *addr = (volatile uint16_t *)(g_notify_base + (uint64_t)q->notify_off * g_notify_off_multiplier);
    __asm__ volatile ("" ::: "memory");
    *addr = (q == &g_rxq) ? VIRTIO_NET_RXQ : VIRTIO_NET_TXQ;
}

static int setup_queue(uint16_t index, virtq_t *q) {
    g_common->queue_select = index;
    uint16_t max_size = g_common->queue_size;
    if (max_size == 0) {
        serial_print("virtio-net: queue unavailable\n");
        return 0;
    }
    q->size = (max_size < VIRTIO_NET_QUEUE_SIZE) ? max_size : VIRTIO_NET_QUEUE_SIZE;

    q->desc_phys  = pmm_alloc_page();
    q->avail_phys = pmm_alloc_page();
    q->used_phys  = pmm_alloc_page();
    if (!q->desc_phys || !q->avail_phys || !q->used_phys) {
        serial_print("virtio-net: out of memory setting up queue\n");
        return 0;
    }

    q->desc  = (virtq_desc_t *)phys_to_virt(q->desc_phys);
    q->avail = (virtq_avail_t *)phys_to_virt(q->avail_phys);
    q->used  = (virtq_used_t *)phys_to_virt(q->used_phys);
    vnet_memset((void *)q->desc, 0, 4096);
    vnet_memset((void *)q->avail, 0, 4096);
    vnet_memset((void *)q->used, 0, 4096);
    q->last_used_idx = 0;

    g_common->queue_size = q->size;
    g_common->queue_desc   = q->desc_phys;
    g_common->queue_driver = q->avail_phys;
    g_common->queue_device = q->used_phys;
    q->notify_off = g_common->queue_notify_off;
    g_common->queue_enable = 1;

    return 1;
}

static void post_rx_buffer(uint16_t slot) {
    virtq_desc_t *d = &g_rxq.desc[slot];
    d->addr = g_rx_buf_phys[slot];
    d->len = VIRTIO_NET_RX_BUF_SIZE;
    d->flags = VIRTQ_DESC_F_WRITE;
    d->next = 0;

    uint16_t ring_idx = g_rxq.avail->idx % g_rxq.size;
    g_rxq.avail->ring[ring_idx] = slot;
    __asm__ volatile ("" ::: "memory");
    g_rxq.avail->idx++;
}

void virtio_net_get_mac(uint8_t mac_out[6]) {
    vnet_memcpy(mac_out, g_mac, 6);
}

int virtio_net_is_ready(void) {
    return g_ready;
}

void virtio_net_init(void) {
    g_found = 0;
    pci_scan(vnet_pci_cb);
    if (!g_found) {
        serial_print("virtio-net: no device found on PCI bus\n");
        return;
    }

    // Enable memory space + bus mastering (required for MMIO + DMA)
    uint32_t cmd_status = pci_config_read32(g_bus, g_dev, g_func, 0x04);
    cmd_status |= 0x0006;
    pci_config_write32(g_bus, g_dev, g_func, 0x04, cmd_status);

    if (!parse_capabilities()) {
        return;
    }

    // Reset device
    g_common->device_status = 0;
    while (g_common->device_status != 0) {}

    g_common->device_status |= VIRTIO_STATUS_ACKNOWLEDGE;
    g_common->device_status |= VIRTIO_STATUS_DRIVER;

    g_common->device_feature_select = 0;
    uint32_t feat_low = g_common->device_feature;
    g_common->device_feature_select = 1;
    uint32_t feat_high = g_common->device_feature;

    uint32_t want_low = feat_low & (1UL << VIRTIO_NET_F_MAC_BIT);
    uint32_t want_high = feat_high & (1UL << VIRTIO_F_VERSION_1_BIT);

    if (!(want_high & (1UL << VIRTIO_F_VERSION_1_BIT))) {
        serial_print("virtio-net: device does not support VERSION_1, aborting\n");
        g_common->device_status |= VIRTIO_STATUS_FAILED;
        return;
    }

    g_common->driver_feature_select = 0;
    g_common->driver_feature = want_low;
    g_common->driver_feature_select = 1;
    g_common->driver_feature = want_high;

    g_common->device_status |= VIRTIO_STATUS_FEATURES_OK;
    if (!(g_common->device_status & VIRTIO_STATUS_FEATURES_OK)) {
        serial_print("virtio-net: feature negotiation rejected by device\n");
        g_common->device_status |= VIRTIO_STATUS_FAILED;
        return;
    }

    if (!setup_queue(VIRTIO_NET_RXQ, &g_rxq)) return;
    if (!setup_queue(VIRTIO_NET_TXQ, &g_txq)) return;

    for (int i = 0; i < g_rxq.size; i++) {
        g_rx_buf_phys[i] = pmm_alloc_page();
        if (!g_rx_buf_phys[i]) {
            serial_print("virtio-net: out of memory allocating rx buffers\n");
            return;
        }
        post_rx_buffer((uint16_t)i);
    }
    notify_queue(&g_rxq);

    g_tx_hdr_phys = pmm_alloc_page();
    if (!g_tx_hdr_phys) {
        serial_print("virtio-net: out of memory allocating tx header buffer\n");
        return;
    }

    if (want_low & (1UL << VIRTIO_NET_F_MAC_BIT)) {
        for (int i = 0; i < 6; i++) g_mac[i] = g_device_cfg[i];
    } else {
        // Device didn't offer a MAC; fall back to a locally-administered address
        uint8_t fallback[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        vnet_memcpy(g_mac, fallback, 6);
    }

    g_common->device_status |= VIRTIO_STATUS_DRIVER_OK;

    // Determine and wire up the legacy PCI interrupt line
    uint32_t irq_dword = pci_config_read32(g_bus, g_dev, g_func, 0x3C);
    g_irq_line = (uint8_t)(irq_dword & 0xFF);

    uint64_t handler_addr = 0;
    switch (g_irq_line) {
        case 5:  handler_addr = (uint64_t)irq5;  break;
        case 9:  handler_addr = (uint64_t)irq9;  break;
        case 10: handler_addr = (uint64_t)irq10; break;
        case 11: handler_addr = (uint64_t)irq11; break;
        default:
            serial_print("virtio-net: unexpected IRQ line assigned; RX interrupts unavailable\n");
            break;
    }

    if (handler_addr) {
        idt_set_gate((uint8_t)(32 + g_irq_line), handler_addr, 0x08, 0x8E);
        pic_unmask_irq(g_irq_line);
    }

    g_ready = 1;
    serial_print("virtio-net: driver ready\n");
}

int virtio_net_send_frame(const uint8_t *frame, uint16_t len) {
    if (!g_ready) return 0;
    if (len > VIRTIO_NET_RX_BUF_SIZE - sizeof(virtio_net_hdr_t)) return 0;

    extern uint64_t vmm_get_phys(uint64_t *pml4, uint64_t virt);
    uint64_t frame_phys = vmm_get_phys(vmm_get_kernel_directory(), (uint64_t)frame);
    serial_print("virtio-net: tx frame vaddr=");
    serial_print_hex((uint64_t)frame);
    serial_print(" resolved phys=");
    serial_print_hex(frame_phys);
    serial_print("\n");
    if (!frame_phys) {
        serial_print("virtio-net: could not resolve physical address for tx buffer\n");
        return 0;
    }

    virtio_net_hdr_t *hdr = (virtio_net_hdr_t *)phys_to_virt(g_tx_hdr_phys);
    vnet_memset(hdr, 0, sizeof(virtio_net_hdr_t));

    g_txq.desc[0].addr = g_tx_hdr_phys;
    g_txq.desc[0].len = sizeof(virtio_net_hdr_t);
    g_txq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    g_txq.desc[0].next = 1;

    g_txq.desc[1].addr = frame_phys;
    g_txq.desc[1].len = len;
    g_txq.desc[1].flags = 0;
    g_txq.desc[1].next = 0;

    uint16_t ring_idx = g_txq.avail->idx % g_txq.size;
    g_txq.avail->ring[ring_idx] = 0;
    __asm__ volatile ("" ::: "memory");
    g_txq.avail->idx++;

    notify_queue(&g_txq);

    uint16_t target = g_txq.last_used_idx + 1;
    for (volatile int i = 0; i < 2000000 && g_txq.used->idx != target; i++) {}
    if (g_txq.used->idx == target) {
        g_txq.last_used_idx = target;
        serial_print("virtio-net: tx confirmed complete\n");
        return 1;
    }
    serial_print("virtio-net: tx timed out\n");
    return 0;
}

void virtio_net_handle_irq(void) {
    volatile uint8_t isr_status = *g_isr; // reading clears it
    serial_print("virtio-net: IRQ fired, isr=");
    serial_print_hex(isr_status);
    serial_print("\n");

    while (g_rxq.last_used_idx != g_rxq.used->idx) {
        uint16_t idx = g_rxq.last_used_idx % g_rxq.size;
        virtq_used_elem_t elem = g_rxq.used->ring[idx];
        uint32_t slot = elem.id;
        uint32_t total_len = elem.len;

        if (total_len > sizeof(virtio_net_hdr_t) && slot < g_rxq.size) {
            uint8_t *buf = (uint8_t *)phys_to_virt(g_rx_buf_phys[slot]);
            extern void eth_handle_frame(const uint8_t *frame, uint16_t len);
            eth_handle_frame(buf + sizeof(virtio_net_hdr_t), (uint16_t)(total_len - sizeof(virtio_net_hdr_t)));
        }

        post_rx_buffer((uint16_t)slot);
        g_rxq.last_used_idx++;
    }

    notify_queue(&g_rxq);
}

void net_irq_dispatch(uint8_t irq) {
    if (irq == g_irq_line) {
        virtio_net_handle_irq();
    }
}

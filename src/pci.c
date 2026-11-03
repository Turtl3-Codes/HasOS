#include "pci.h"
#include <stddef.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t address = (1UL << 31)
                      | ((uint32_t)bus << 16)
                      | ((uint32_t)device << 11)
                      | ((uint32_t)function << 8)
                      | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    uint32_t address = (1UL << 31)
                      | ((uint32_t)bus << 16)
                      | ((uint32_t)device << 11)
                      | ((uint32_t)function << 8)
                      | (offset & 0xFC);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_index) {
    uint8_t offset = 0x10 + (bar_index * 4);
    uint32_t bar = pci_config_read32(bus, device, function, offset);
    // Mask off the low info bits: bit0 = space (0=mem), bits1-2 = type,
    // bit3 = prefetchable. For a 32-bit memory BAR the low 4 bits are flags.
    return bar & ~0xFUL;
}

static uint16_t pci_get_vendor_id(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t reg0 = pci_config_read32(bus, device, function, 0x00);
    return (uint16_t)(reg0 & 0xFFFF);
}

static void pci_scan_function(uint8_t bus, uint8_t device, uint8_t function, pci_scan_cb callback) {
    uint16_t vendor_id = pci_get_vendor_id(bus, device, function);
    if (vendor_id == 0xFFFF) {
        return; // No device present
    }

    uint32_t reg0 = pci_config_read32(bus, device, function, 0x00);
    uint32_t reg2 = pci_config_read32(bus, device, function, 0x08);

    pci_device_t dev;
    dev.bus        = bus;
    dev.device     = device;
    dev.function   = function;
    dev.vendor_id  = (uint16_t)(reg0 & 0xFFFF);
    dev.device_id  = (uint16_t)((reg0 >> 16) & 0xFFFF);
    dev.prog_if    = (uint8_t)((reg2 >> 8) & 0xFF);
    dev.subclass   = (uint8_t)((reg2 >> 16) & 0xFF);
    dev.class_code = (uint8_t)((reg2 >> 24) & 0xFF);

    callback(&dev);
}

static void pci_scan_device(uint8_t bus, uint8_t device, pci_scan_cb callback) {
    if (pci_get_vendor_id(bus, device, 0) == 0xFFFF) {
        return; // No device at function 0 means nothing present
    }

    pci_scan_function(bus, device, 0, callback);

    uint32_t reg3 = pci_config_read32(bus, device, 0, 0x0C);
    uint8_t header_type = (uint8_t)((reg3 >> 16) & 0xFF);
    if (header_type & 0x80) {
        // Multi-function device: scan functions 1-7
        for (uint8_t function = 1; function < 8; function++) {
            pci_scan_function(bus, device, function, callback);
        }
    }
}

void pci_scan(pci_scan_cb callback) {
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            pci_scan_device((uint8_t)bus, device, callback);
        }
    }
}

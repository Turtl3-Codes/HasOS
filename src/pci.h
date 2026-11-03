#ifndef PCI_H
#define PCI_H

#include <stdint.h>

typedef struct {
    uint8_t  bus;
    uint8_t  device;
    uint8_t  function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
} pci_device_t;

// Scans all PCI buses/devices/functions. Calls back for each device found.
typedef void (*pci_scan_cb)(const pci_device_t *dev);
void pci_scan(pci_scan_cb callback);

// Reads a BAR (0-5) for a given device, masking off the low flag bits.
// Only handles 32-bit memory BARs for now (sufficient for AHCI's BAR5).
uint32_t pci_read_bar(uint8_t bus, uint8_t device, uint8_t function, uint8_t bar_index);

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

#endif

#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

typedef enum {
    AHCI_DEV_NONE,
    AHCI_DEV_SATA,
    AHCI_DEV_SATAPI,
    AHCI_DEV_SEMB,
    AHCI_DEV_PM
} ahci_dev_type_t;

typedef struct {
    int port_number;
    ahci_dev_type_t type;
} ahci_port_info_t;

// Scans PCI for the AHCI controller, maps its ABAR, enables AHCI mode.
// Returns 1 on success, 0 if no AHCI controller was found.
int ahci_init(void);

// Calls back once for every implemented port with a device physically
// present, after a successful ahci_init(). Returns the number of ports
// reported, or -1 if ahci_init() hasn't been called successfully yet.
typedef void (*ahci_port_cb)(const ahci_port_info_t *info);
int ahci_scan_ports(ahci_port_cb callback);

// Reads 'count' sectors (512 bytes each, count <= 8) starting at 'lba'
// from the given port into 'dest' (must be at least count*512 bytes).
// Lazily initializes the port's command structures on first use.
// Returns 1 on success, 0 on failure or timeout.
int ahci_read_sectors(int port_number, uint64_t lba, uint16_t count, uint8_t *dest);

// Writes 'count' sectors (512 bytes each, count <= 8) starting at 'lba'
// on the given port from 'src' (must be at least count*512 bytes).
// Lazily initializes the port's command structures on first use.
// Returns 1 on success, 0 on failure or timeout.
int ahci_write_sectors(int port_number, uint64_t lba, uint16_t count, const uint8_t *src);

#endif

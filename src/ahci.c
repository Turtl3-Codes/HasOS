#include "ahci.h"
#include "pci.h"
#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *str);
extern uint64_t *vmm_get_kernel_directory(void);
extern void vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
extern uint64_t pmm_alloc_page(void);
extern uint64_t pmm_get_hhdm_offset(void);

// --- HBA memory-mapped register layout (AHCI spec 1.3.1) ---

typedef struct {
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, rsv0;
    uint32_t tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) hba_port_t;

typedef struct {
    uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} __attribute__((packed)) hba_mem_t;

// --- Command list / FIS / command table layout (AHCI spec 4.2) ---

typedef struct {
    uint8_t  cfl:5;    // Command FIS length in dwords
    uint8_t  a:1;      // ATAPI
    uint8_t  w:1;      // Write, 1 = H2D
    uint8_t  p:1;      // Prefetchable
    uint8_t  r:1;      // Reset
    uint8_t  b:1;      // BIST
    uint8_t  c:1;      // Clear busy on R_OK
    uint8_t  rsv0:1;
    uint8_t  pmp:4;    // Port multiplier port
    uint16_t prdtl;    // PRDT entry count
    volatile uint32_t prdbc; // bytes transferred
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint8_t  fis_type;
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;      // 1 = command, 0 = control
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2;
    uint8_t  device;
    uint8_t  lba3, lba4, lba5;
    uint8_t  featureh;
    uint8_t  countl, counth;
    uint8_t  icc;
    uint8_t  control;
    uint8_t  rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;   // byte count - 1
    uint32_t rsv1:9;
    uint32_t i:1;      // interrupt on completion
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt_entry[1];
} __attribute__((packed)) hba_cmd_tbl_t;

#define FIS_TYPE_REG_H2D 0x27
#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35

#define GHC_AE (1UL << 31)

#define SSTS_DET_MASK    0xF
#define SSTS_DET_PRESENT 0x3

#define SIG_ATA    0x00000101
#define SIG_ATAPI  0xEB140101
#define SIG_SEMB   0xC33C0101
#define SIG_PM     0x96690101

#define AHCI_CLASS    0x01
#define AHCI_SUBCLASS 0x06
#define AHCI_PROGIF   0x01

#define AHCI_ABAR_VADDR 0xFFFFA00000000000UL
#define AHCI_ABAR_PAGES 4

#define PxCMD_ST  (1UL << 0)
#define PxCMD_SUD (1UL << 1)
#define PxCMD_POD (1UL << 2)
#define PxCMD_FRE (1UL << 4)
#define PxCMD_FR  (1UL << 14)
#define PxCMD_CR  (1UL << 15)

#define ATA_DEV_BUSY 0x80
#define ATA_DEV_DRQ  0x08

#define PxIS_TFES (1UL << 30)

#define SPIN_TIMEOUT 1000000

static hba_mem_t *g_hba = NULL;
static int g_hba_ready = 0;
static int g_ahci_found = 0;
static uint8_t g_ahci_bus, g_ahci_device, g_ahci_function;

typedef struct {
    int initialized;
    uint64_t clb_phys;
    uint64_t fb_phys;
    uint64_t ctba_phys;
    uint64_t buf_phys; // scratch DMA data buffer, one 4KB page (8 sectors)
} ahci_port_state_t;

static ahci_port_state_t g_port_state[32];

static inline uint64_t phys_to_virt(uint64_t phys) {
    return phys + pmm_get_hhdm_offset();
}

static void ahci_memset(void *dst, uint8_t val, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = val;
    }
}

static void ahci_memcpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

static void ahci_pci_cb(const pci_device_t *dev) {
    if (g_ahci_found) {
        return; // keep the first controller found
    }
    if (dev->class_code == AHCI_CLASS && dev->subclass == AHCI_SUBCLASS && dev->prog_if == AHCI_PROGIF) {
        g_ahci_found = 1;
        g_ahci_bus = dev->bus;
        g_ahci_device = dev->device;
        g_ahci_function = dev->function;
    }
}

static ahci_dev_type_t ahci_sig_type(uint32_t sig) {
    switch (sig) {
        case SIG_ATA:   return AHCI_DEV_SATA;
        case SIG_ATAPI: return AHCI_DEV_SATAPI;
        case SIG_SEMB:  return AHCI_DEV_SEMB;
        case SIG_PM:    return AHCI_DEV_PM;
        default:        return AHCI_DEV_NONE;
    }
}

int ahci_init(void) {
    g_hba_ready = 0;
    g_ahci_found = 0;
    pci_scan(ahci_pci_cb);

    if (!g_ahci_found) {
        serial_print("AHCI: no controller found on PCI bus\n");
        return 0;
    }

    uint32_t abar_phys = pci_read_bar(g_ahci_bus, g_ahci_device, g_ahci_function, 5);
    if (abar_phys == 0) {
        serial_print("AHCI: BAR5 read as 0, cannot map\n");
        return 0;
    }

    uint64_t *pml4 = vmm_get_kernel_directory();
    for (int i = 0; i < AHCI_ABAR_PAGES; i++) {
        vmm_map_page(pml4,
                      AHCI_ABAR_VADDR + (uint64_t)i * 4096,
                      (uint64_t)abar_phys + (uint64_t)i * 4096,
                      PAGE_PRESENT | PAGE_WRITE);
    }

    g_hba = (hba_mem_t *)AHCI_ABAR_VADDR;

    if (!(g_hba->ghc & GHC_AE)) {
        g_hba->ghc |= GHC_AE;
    }

    for (int i = 0; i < 32; i++) {
        g_port_state[i].initialized = 0;
    }

    g_hba_ready = 1;
    return 1;
}

int ahci_scan_ports(ahci_port_cb callback) {
    if (!g_hba_ready) {
        return -1;
    }

    int count = 0;
    uint32_t pi = g_hba->pi;

    for (int port = 0; port < 32; port++) {
        if (!(pi & (1UL << port))) {
            continue;
        }

        hba_port_t *p = &g_hba->ports[port];
        if ((p->ssts & SSTS_DET_MASK) != SSTS_DET_PRESENT) {
            continue;
        }

        ahci_port_info_t info;
        info.port_number = port;
        info.type = ahci_sig_type(p->sig);
        callback(&info);
        count++;
    }

    return count;
}

static int ahci_port_init(int port_number) {
    if (!g_hba_ready) {
        return 0;
    }

    hba_port_t *p = &g_hba->ports[port_number];
    ahci_port_state_t *st = &g_port_state[port_number];

    // Stop the command engine before touching CLB/FB.
    p->cmd &= ~PxCMD_ST;
    for (int i = 0; (p->cmd & PxCMD_CR) && i < SPIN_TIMEOUT; i++) {}
    p->cmd &= ~PxCMD_FRE;
    for (int i = 0; (p->cmd & PxCMD_FR) && i < SPIN_TIMEOUT; i++) {}

    st->clb_phys = pmm_alloc_page();
    st->fb_phys = pmm_alloc_page();
    st->ctba_phys = pmm_alloc_page();
    st->buf_phys = pmm_alloc_page();
    if (!st->clb_phys || !st->fb_phys || !st->ctba_phys || !st->buf_phys) {
        serial_print("AHCI: out of memory initializing port\n");
        return 0;
    }

    ahci_memset((void *)phys_to_virt(st->clb_phys), 0, 4096);
    ahci_memset((void *)phys_to_virt(st->fb_phys), 0, 4096);
    ahci_memset((void *)phys_to_virt(st->ctba_phys), 0, 4096);

    p->clb  = (uint32_t)(st->clb_phys & 0xFFFFFFFF);
    p->clbu = (uint32_t)(st->clb_phys >> 32);
    p->fb   = (uint32_t)(st->fb_phys & 0xFFFFFFFF);
    p->fbu  = (uint32_t)(st->fb_phys >> 32);

    hba_cmd_header_t *cmd_header = (hba_cmd_header_t *)phys_to_virt(st->clb_phys);
    cmd_header[0].ctba  = (uint32_t)(st->ctba_phys & 0xFFFFFFFF);
    cmd_header[0].ctbau = (uint32_t)(st->ctba_phys >> 32);

    p->serr = 0xFFFFFFFF; // clear any pending errors (write-1-to-clear)

    p->cmd |= PxCMD_FRE;
    p->cmd |= PxCMD_SUD | PxCMD_POD;

    for (int i = 0; (p->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && i < SPIN_TIMEOUT; i++) {}

    p->cmd |= PxCMD_ST;

    st->initialized = 1;
    return 1;
}

int ahci_read_sectors(int port_number, uint64_t lba, uint16_t count, uint8_t *dest) {
    if (!g_hba_ready || port_number < 0 || port_number >= 32 || count == 0 || count > 8) {
        return 0;
    }

    ahci_port_state_t *st = &g_port_state[port_number];
    if (!st->initialized) {
        if (!ahci_port_init(port_number)) {
            return 0;
        }
    }

    hba_port_t *p = &g_hba->ports[port_number];

    for (int i = 0; (p->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && i < SPIN_TIMEOUT; i++) {}

    hba_cmd_header_t *cmd_header = (hba_cmd_header_t *)phys_to_virt(st->clb_phys);
    cmd_header[0].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header[0].w = 0;
    cmd_header[0].a = 0;
    cmd_header[0].prdtl = 1;
    cmd_header[0].prdbc = 0;

    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)phys_to_virt(st->ctba_phys);
    ahci_memset(tbl, 0, sizeof(hba_cmd_tbl_t));

    tbl->prdt_entry[0].dba  = (uint32_t)(st->buf_phys & 0xFFFFFFFF);
    tbl->prdt_entry[0].dbau = (uint32_t)(st->buf_phys >> 32);
    tbl->prdt_entry[0].dbc  = (uint32_t)count * 512 - 1;
    tbl->prdt_entry[0].i    = 0;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_READ_DMA_EX;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 0x40; // LBA mode
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    p->is = 0xFFFFFFFF; // clear pending interrupt status

    p->ci |= (1UL << 0); // issue on slot 0

    int timed_out = 1;
    for (int i = 0; i < SPIN_TIMEOUT; i++) {
        if (!(p->ci & (1UL << 0))) {
            timed_out = 0;
            break;
        }
        if (p->is & PxIS_TFES) {
            serial_print("AHCI: task file error during read\n");
            return 0;
        }
    }

    if (timed_out) {
        serial_print("AHCI: read timed out\n");
        return 0;
    }

    ahci_memcpy(dest, (void *)phys_to_virt(st->buf_phys), (uint64_t)count * 512);
    return 1;
}

int ahci_write_sectors(int port_number, uint64_t lba, uint16_t count, const uint8_t *src) {
    if (!g_hba_ready || port_number < 0 || port_number >= 32 || count == 0 || count > 8) {
        return 0;
    }

    ahci_port_state_t *st = &g_port_state[port_number];
    if (!st->initialized) {
        if (!ahci_port_init(port_number)) {
            return 0;
        }
    }

    hba_port_t *p = &g_hba->ports[port_number];

    for (int i = 0; (p->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && i < SPIN_TIMEOUT; i++) {}

    // Copy caller's data into the DMA scratch buffer before issuing the
    // command, since the HBA reads from physical memory during the
    // transfer and we don't control 'src's physical address directly.
    ahci_memcpy((void *)phys_to_virt(st->buf_phys), src, (uint64_t)count * 512);

    hba_cmd_header_t *cmd_header = (hba_cmd_header_t *)phys_to_virt(st->clb_phys);
    cmd_header[0].cfl = sizeof(fis_reg_h2d_t) / sizeof(uint32_t);
    cmd_header[0].w = 1; // write: HBA -> device
    cmd_header[0].a = 0;
    cmd_header[0].prdtl = 1;
    cmd_header[0].prdbc = 0;

    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)phys_to_virt(st->ctba_phys);
    ahci_memset(tbl, 0, sizeof(hba_cmd_tbl_t));

    tbl->prdt_entry[0].dba  = (uint32_t)(st->buf_phys & 0xFFFFFFFF);
    tbl->prdt_entry[0].dbau = (uint32_t)(st->buf_phys >> 32);
    tbl->prdt_entry[0].dbc  = (uint32_t)count * 512 - 1;
    tbl->prdt_entry[0].i    = 0;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_WRITE_DMA_EX;
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->device = 0x40; // LBA mode
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    fis->countl = (uint8_t)(count & 0xFF);
    fis->counth = (uint8_t)((count >> 8) & 0xFF);

    p->is = 0xFFFFFFFF;

    p->ci |= (1UL << 0);

    int timed_out = 1;
    for (int i = 0; i < SPIN_TIMEOUT; i++) {
        if (!(p->ci & (1UL << 0))) {
            timed_out = 0;
            break;
        }
        if (p->is & PxIS_TFES) {
            serial_print("AHCI: task file error during write\n");
            return 0;
        }
    }

    if (timed_out) {
        serial_print("AHCI: write timed out\n");
        return 0;
    }

    return 1;
}

#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

extern uint64_t pmm_alloc_page(void);
extern uint64_t pmm_get_hhdm_offset(void);
extern void serial_print(const char *str);

static uint64_t *kernel_pml4      = NULL;
static uint64_t  kernel_pml4_phys = 0;

#define PHYS_TO_VIRT(p) ((uint64_t *)((p) + pmm_get_hhdm_offset()))
#define PAGE_PS (1UL << 7)

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

uint64_t *vmm_get_kernel_directory(void) {
    return kernel_pml4;
}

void vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        uint64_t new_pdpt = pmm_alloc_page();
        pml4[pml4_idx] = new_pdpt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else if (flags & PAGE_USER) {
        pml4[pml4_idx] |= PAGE_USER;
    }
    uint64_t *pdpt = PHYS_TO_VIRT(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        uint64_t new_pd = pmm_alloc_page();
        pdpt[pdpt_idx] = new_pd | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else if (flags & PAGE_USER) {
        pdpt[pdpt_idx] |= PAGE_USER;
    }
    uint64_t *pd = PHYS_TO_VIRT(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        uint64_t new_pt = pmm_alloc_page();
        pd[pd_idx] = new_pt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else if (flags & PAGE_USER) {
        pd[pd_idx] |= PAGE_USER;
    }
    uint64_t *pt = PHYS_TO_VIRT(pd[pd_idx] & ~0xFFFUL);
    pt[pt_idx] = (phys & ~0xFFFUL) | flags;
}

void vmm_unmap_page(uint64_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        return;
    }
    uint64_t *pdpt = PHYS_TO_VIRT(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT) || (pdpt[pdpt_idx] & PAGE_PS)) {
        return;
    }
    uint64_t *pd = PHYS_TO_VIRT(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & PAGE_PRESENT) || (pd[pd_idx] & PAGE_PS)) {
        return;
    }
    uint64_t *pt = PHYS_TO_VIRT(pd[pd_idx] & ~0xFFFUL);
    pt[pt_idx] = 0;

    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_init(void) {
    kernel_pml4_phys = read_cr3() & ~0xFFFUL;
    kernel_pml4 = PHYS_TO_VIRT(kernel_pml4_phys);
    serial_print("[OK] VMM initialized (extending Limine's page tables)\n");
}

// Returns the physical address mapped at 'virt' in 'pml4', or 0 if
// unmapped at any level. Does not allocate -- pure lookup. Handles
// 2MB (PD-level) and 1GB (PDPT-level) huge pages, since Limine
// typically maps the kernel image using 2MB pages rather than 4KB.
uint64_t vmm_get_phys(uint64_t *pml4, uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    uint64_t page_off = virt & 0xFFFUL;

    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        return 0;
    }
    uint64_t *pdpt = PHYS_TO_VIRT(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        return 0;
    }
    if (pdpt[pdpt_idx] & PAGE_PS) {
        uint64_t base = pdpt[pdpt_idx] & ~0x3FFFFFFFUL;
        return base | (virt & 0x3FFFFFFFUL);
    }
    uint64_t *pd = PHYS_TO_VIRT(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        return 0;
    }
    if (pd[pd_idx] & PAGE_PS) {
        uint64_t base = pd[pd_idx] & ~0x1FFFFFUL;
        return base | (virt & 0x1FFFFFUL);
    }
    uint64_t *pt = PHYS_TO_VIRT(pd[pd_idx] & ~0xFFFUL);
    if (!(pt[pt_idx] & PAGE_PRESENT)) {
        return 0;
    }
    return (pt[pt_idx] & 0x000FFFFFFFFFF000UL) | page_off;
}

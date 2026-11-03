#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_PRESENT (1 << 0)
#define PAGE_WRITE   (1 << 1)
#define PAGE_USER    (1 << 2)

// Compatibility aliases for legacy kernel code
#define PTE_PRESENT  PAGE_PRESENT
#define PTE_WRITABLE PAGE_WRITE
#define PTE_USER     PAGE_USER

void vmm_init(void);
uint64_t *vmm_get_kernel_directory(void);
void vmm_map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
uint64_t vmm_get_phys(uint64_t *pml4, uint64_t virt);

// Clears the leaf PTE mapping 'virt' (if present) and flushes the TLB
// entry for it. Does not touch or free the underlying physical page --
// callers that own it separately (e.g. process_wipe_current()) do that.
void vmm_unmap_page(uint64_t *pml4, uint64_t virt);

// Helper wrapper to match 3-argument calls targeting the active kernel directory
static inline void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    vmm_map_page(vmm_get_kernel_directory(), virt, phys, flags);
}

#endif

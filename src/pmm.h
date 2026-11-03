#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

void pmm_init(void);
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys_addr);
uint64_t pmm_get_free_memory(void);
uint64_t pmm_get_hhdm_offset(void);

// Zeroes one physical page via its HHDM mapping. Used to scrub a
// single process's memory (stack, ELF segments) at exit time, before
// the page goes back to the free pool. See pmm_secure_wipe() for the
// whole-system equivalent run at shutdown.
void pmm_wipe_page(uint64_t phys_addr);

// Zeroes all USABLE RAM. See the comment above the implementation in
// pmm.c -- this must be the absolute last memory-touching operation
// before halt/power-off.
void pmm_secure_wipe(void);

#endif

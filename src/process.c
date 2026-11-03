#include "process.h"
#include "vmm.h"
#include "elf.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *str);
extern void pmm_free_page(uint64_t phys_addr);
extern void pmm_wipe_page(uint64_t phys_addr);

#define PROCESS_MAX_PAGES 256

static uint64_t g_stack_vaddr = 0;
static uint64_t g_stack_phys  = 0;
static int      g_has_stack   = 0;

void process_register_stack(uint64_t vaddr, uint64_t phys) {
    g_stack_vaddr = vaddr;
    g_stack_phys  = phys;
    g_has_stack   = 1;
}

void process_wipe_current(void) {
    uint64_t *pml4 = vmm_get_kernel_directory();

    uint64_t vaddrs[PROCESS_MAX_PAGES];
    uint64_t physaddrs[PROCESS_MAX_PAGES];
    int count = elf_get_loaded_pages(vaddrs, physaddrs, PROCESS_MAX_PAGES);

    for (int i = 0; i < count; i++) {
        pmm_wipe_page(physaddrs[i]);
        vmm_unmap_page(pml4, vaddrs[i]);
        pmm_free_page(physaddrs[i]);
    }

    if (g_has_stack) {
        pmm_wipe_page(g_stack_phys);
        vmm_unmap_page(pml4, g_stack_vaddr);
        pmm_free_page(g_stack_phys);
        g_has_stack = 0;
    }

    serial_print("[process] user process memory wiped and reclaimed\n");
}

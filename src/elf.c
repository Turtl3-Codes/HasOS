#include "vmm.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *str);
extern uint64_t pmm_alloc_page(void);

#define PT_LOAD 1

#define PF_X 1
#define PF_W 2
#define PF_R 4

#define ELFCLASS64  2
#define EM_X86_64   62

typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

#define ELF_MAX_TRACKED_PAGES 256
static uint64_t g_loaded_vaddr[ELF_MAX_TRACKED_PAGES];
static uint64_t g_loaded_phys[ELF_MAX_TRACKED_PAGES];
static int g_loaded_count = 0;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

uint64_t elf_load(const uint8_t *elf_data) {
    g_loaded_count = 0;

    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)elf_data;

    if (ehdr->e_ident[0] != 0x7F || ehdr->e_ident[1] != 'E' ||
        ehdr->e_ident[2] != 'L' || ehdr->e_ident[3] != 'F') {
        serial_print("ELF: Invalid magic numbers!\n");
        return 0;
    }

    if (ehdr->e_ident[4] != ELFCLASS64) {
        serial_print("ELF: Not a 64-bit ELF (ELFCLASS64 expected)!\n");
        return 0;
    }

    if (ehdr->e_machine != EM_X86_64) {
        serial_print("ELF: Wrong architecture (expected x86_64)!\n");
        return 0;
    }

    uint64_t *pml4 = vmm_get_kernel_directory();
    Elf64_Phdr *phdr = (Elf64_Phdr *)(elf_data + ehdr->e_phoff);

    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type != PT_LOAD) {
            continue;
        }

        uint64_t vaddr  = phdr[i].p_vaddr;
        uint64_t memsz  = phdr[i].p_memsz;
        uint64_t filesz = phdr[i].p_filesz;
        uint64_t offset = phdr[i].p_offset;

        uint64_t final_flags = PAGE_PRESENT | PAGE_USER;
        if (phdr[i].p_flags & PF_W) {
            final_flags |= PAGE_WRITE;
        }
        // No NX bit support yet, so PF_X is implicit; nothing to add.

        uint64_t page_start = vaddr & ~0xFFFUL;
        uint64_t page_end   = (vaddr + memsz + 0xFFFUL) & ~0xFFFUL;

        // Pass 1: map every page WRITABLE regardless of the segment's
        // real permissions, so the kernel-side copy below can write
        // through it. Reuse pages already mapped by an earlier segment
        // sharing this page instead of allocating a fresh one.
        for (uint64_t page_vaddr = page_start; page_vaddr < page_end; page_vaddr += 4096) {
            uint64_t existing_phys = vmm_get_phys(pml4, page_vaddr);
            if (existing_phys != 0) {
                vmm_map_page(pml4, page_vaddr, existing_phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);
                continue;
            }

            uint64_t phys_page = pmm_alloc_page();
            if (phys_page == 0) {
                serial_print("ELF: out of physical memory while mapping segment!\n");
                return 0;
            }
            vmm_map_page(pml4, page_vaddr, phys_page, PAGE_PRESENT | PAGE_WRITE | PAGE_USER);

            if (g_loaded_count < ELF_MAX_TRACKED_PAGES) {
                g_loaded_vaddr[g_loaded_count] = page_vaddr;
                g_loaded_phys[g_loaded_count]  = phys_page;
                g_loaded_count++;
            } else {
                serial_print("ELF: warning - too many pages to track for wipe-on-exit\n");
            }
        }

        uint8_t *dest = (uint8_t *)vaddr;
        const uint8_t *src = elf_data + offset;
        for (uint64_t b = 0; b < filesz; b++) {
            dest[b] = src[b];
        }
        for (uint64_t b = filesz; b < memsz; b++) {
            dest[b] = 0;
        }

        // Pass 2: downgrade each page to the segment's real permissions
        // now that the copy is done.
        if (!(final_flags & PAGE_WRITE)) {
            for (uint64_t page_vaddr = page_start; page_vaddr < page_end; page_vaddr += 4096) {
                uint64_t phys = vmm_get_phys(pml4, page_vaddr);
                vmm_map_page(pml4, page_vaddr, phys, final_flags);
            }
        }
    }

    return ehdr->e_entry;
}

int elf_get_loaded_pages(uint64_t *out_vaddr, uint64_t *out_phys, int max) {
    int n = g_loaded_count;
    if (n > max) {
        n = max;
    }
    for (int i = 0; i < n; i++) {
        out_vaddr[i] = g_loaded_vaddr[i];
        out_phys[i]  = g_loaded_phys[i];
    }
    return n;
}

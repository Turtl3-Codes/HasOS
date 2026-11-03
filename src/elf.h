#ifndef ELF_H
#define ELF_H

#include <stdint.h>

// Parses an in-memory ELF64 image, maps its PT_LOAD segments into
// the current page tables (user-accessible), and returns the entry
// point address to jump to. Returns 0 on failure.
uint64_t elf_load(const uint8_t *image);

// Fills out_vaddr/out_phys (parallel arrays, up to 'max' entries) with
// the pages elf_load() allocated for the most recently loaded program,
// and returns how many entries were written. Used by
// process_wipe_current() to scrub those pages on exit.
int elf_get_loaded_pages(uint64_t *out_vaddr, uint64_t *out_phys, int max);

#endif

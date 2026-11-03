#include "pmm.h"
#include "serial.h"
#include "limine.h"
#include <stdint.h>
#include <stddef.h>

static uint8_t *bitmap = NULL;
static uint64_t total_pages = 0;
static uint64_t free_pages = 0;
static uint64_t bitmap_size = 0;
static uint64_t hhdm_offset = 0xFFFF800000000000UL;

static inline void bitmap_set(uint64_t bit) {
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_clear(uint64_t bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline int bitmap_test(uint64_t bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_init(void) {
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    }

    struct limine_memmap_response *memmap = memmap_request.response;
    if (memmap == NULL || memmap->entry_count == 0) {
        serial_print("[PMM ERROR] Failed to fetch Limine memory map!\n");
        return;
    }

    uint64_t highest_addr = 0;

    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        uint64_t top = entry->base + entry->length;
        if (top > highest_addr) {
            highest_addr = top;
        }
    }

    total_pages = highest_addr / PAGE_SIZE;
    bitmap_size = total_pages / 8;

    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= bitmap_size) {
            bitmap = (uint8_t *)(entry->base + hhdm_offset);
            break;
        }
    }

    if (!bitmap) {
        serial_print("[PMM ERROR] Could not allocate memory for PMM bitmap!\n");
        return;
    }

    for (uint64_t i = 0; i < bitmap_size; i++) {
        bitmap[i] = 0xFF;
    }

    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t start_page = entry->base / PAGE_SIZE;
            uint64_t pages = entry->length / PAGE_SIZE;

            for (uint64_t p = 0; p < pages; p++) {
                bitmap_clear(start_page + p);
                free_pages++;
            }
        }
    }

    uint64_t bitmap_phys = (uint64_t)bitmap - hhdm_offset;
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = 0; i < bitmap_pages; i++) {
        uint64_t page = (bitmap_phys / PAGE_SIZE) + i;
        if (!bitmap_test(page)) {
            bitmap_set(page);
            free_pages--;
        }
    }

    serial_print("[OK] PMM Bitmap Allocator Initialized Successfully\n");
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            return i * PAGE_SIZE;
        }
    }
    serial_print("[PMM ERROR] Out of physical memory!\n");
    return 0;
}

void pmm_free_page(uint64_t phys_addr) {
    uint64_t page = phys_addr / PAGE_SIZE;
    if (page < total_pages && bitmap_test(page)) {
        bitmap_clear(page);
        free_pages++;
    }
}

void pmm_wipe_page(uint64_t phys_addr) {
    uint8_t *page = (uint8_t *)(phys_addr + hhdm_offset);
    for (uint64_t off = 0; off < PAGE_SIZE; off++) {
        page[off] = 0;
    }
}

uint64_t pmm_get_free_memory(void) {
    return free_pages * PAGE_SIZE;
}

uint64_t pmm_get_hhdm_offset(void) {
    return hhdm_offset;
}

// Zeroes every byte of every USABLE memory region (both currently
// allocated and currently free pages), to remove any remnant of
// sensitive data before power-off -- e.g. file contents that were read
// into RAM, network packet buffers, previously-freed pages that still
// hold stale data, etc.
//
// This deliberately walks Limine's memmap directly rather than the PMM
// bitmap, and only touches USABLE entries: the running kernel's own
// code/stack (type KERNEL_AND_MODULES) and the framebuffer (type
// FRAMEBUFFER) are separate memmap types, so they're safely excluded --
// we don't want to zero the code we're currently executing on, or the
// screen, mid-wipe.
//
// CRITICAL: after this runs, the PMM bitmap itself (which lives inside
// a USABLE region) has been zeroed too, which makes every page look
// "free" again. This function must be the last thing that touches
// memory before halting/powering off -- calling pmm_alloc_page() or
// anything that allocates after this would be unsafe.
void pmm_secure_wipe(void) {
    struct limine_memmap_response *memmap = memmap_request.response;
    if (memmap == NULL) {
        return;
    }

    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        uint8_t *region = (uint8_t *)(entry->base + hhdm_offset);
        for (uint64_t off = 0; off < entry->length; off++) {
            region[off] = 0;
        }
    }
}

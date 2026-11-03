#include "heap.h"
#include "pmm.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define HEAP_INITIAL_PAGES 16

typedef struct heap_block {
    size_t size;
    int is_free;
    struct heap_block *next;
} heap_block_t;

static heap_block_t *heap_start = NULL;

void heap_init(void) {
    uint64_t phys_addr = pmm_alloc_page();
    
    // Explicitly cast physical uint64_t address to virtual pointer (using Limine HHDM offset)
    void *virt_addr = (void *)(phys_addr + 0xFFFF800000000000UL);

    if (phys_addr == 0) {
        serial_print("[HEAP ERROR] Could not allocate initial heap page!\n");
        return;
    }

    heap_start = (heap_block_t *)virt_addr;
    heap_start->size = PAGE_SIZE - sizeof(heap_block_t);
    heap_start->is_free = 1;
    heap_start->next = NULL;

    serial_print("[OK] Kernel Heap Initialized\n");
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    heap_block_t *current = heap_start;
    while (current) {
        if (current->is_free && current->size >= size) {
            current->is_free = 0;
            return (void *)((uint8_t *)current + sizeof(heap_block_t));
        }
        current = current->next;
    }

    serial_print("[HEAP ERROR] Out of heap memory!\n");
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
    block->is_free = 1;
}

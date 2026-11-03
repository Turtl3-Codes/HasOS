#include "task.h"
#include "heap.h"
#include <stddef.h>

extern void serial_print(const char *str);

static struct task tasks[MAX_TASKS];
static int current_task = 0;
static int task_count = 0;

static uint64_t setup_initial_stack(uint8_t *stack_top, void (*entry)(void)) {
    // 16-byte alignment enforce
    uint64_t *sp = (uint64_t *)((uint64_t)stack_top & ~0x0FUL);

    sp--;
    *sp = 0x10;                // SS - kernel data selector

    sp--;
    *sp = (uint64_t)(sp + 1);  // RSP

    sp--;
    *sp = 0x202;               // RFLAGS - IF=1 (Interrupts enabled)

    sp--;
    *sp = 0x08;                // CS - kernel code selector

    sp--;
    *sp = (uint64_t)entry;     // RIP - Task entry point

    sp--;
    *sp = 0;                   // err_code
    sp--;
    *sp = 0;                   // irq number

    // 15 General Purpose Registers (RAX - R15)
    for (int i = 0; i < 15; i++) {
        sp--;
        *sp = 0;
    }

    return (uint64_t)sp;
}

void tasking_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].active = 0;
        tasks[i].rsp = 0;
        tasks[i].stack_base = NULL;
    }

    // Register primary executing kernel thread as Task 0
    tasks[0].active = 1;
    tasks[0].rsp = 0;
    tasks[0].stack_base = NULL;

    current_task = 0;
    task_count = 1;

    serial_print("Tasking initialized (Main Kernel Thread registered as Task 0)\n");
}

int task_create(void (*entry)(void)) {
    if (task_count >= MAX_TASKS) {
        serial_print("TASK: max tasks reached\n");
        return -1;
    }

    uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SIZE);
    if (!stack) {
        serial_print("TASK: failed to allocate stack\n");
        return -1;
    }

    uint8_t *stack_top = stack + TASK_STACK_SIZE;
    int id = task_count++;
    
    tasks[id].stack_base = stack;
    tasks[id].rsp = setup_initial_stack(stack_top, entry);
    tasks[id].active = 1;

    return id;
}

uint64_t schedule(uint64_t current_rsp) {
    if (task_count <= 1) {
        return current_rsp;
    }

    // Save stack pointer of current task
    tasks[current_task].rsp = current_rsp;

    // Pick next active task
    do {
        current_task = (current_task + 1) % task_count;
    } while (!tasks[current_task].active);

    return tasks[current_task].rsp;
}

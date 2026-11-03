#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define MAX_TASKS 8
#define TASK_STACK_SIZE 16384

struct task {
    uint64_t rsp;
    int active;
    uint8_t *stack_base;
};

void tasking_init(void);
int task_create(void (*entry)(void));
uint64_t schedule(uint64_t current_rsp);

#endif

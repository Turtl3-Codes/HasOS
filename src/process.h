#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

// Records the physical page backing the user stack so
// process_wipe_current() can zero/unmap/free it on exit.
void process_register_stack(uint64_t vaddr, uint64_t phys);

// Zeroes, unmaps, and frees every physical page belonging to the user
// process that just exited (ELF segments + stack). Call this before
// returning control to the kernel on SYS_EXIT, so no leftover process
// data -- typed input, decrypted buffers, whatever it had in RAM --
// lingers around for the next thing that runs.
void process_wipe_current(void);

#endif

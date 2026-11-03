#ifndef KERNEL_CTX_H
#define KERNEL_CTX_H

// Saves current kernel context (rsp/rbp/callee-saved regs/return address).
// Returns 0 on the initial call. If kernel_ctx_restore() is later invoked,
// execution resumes here and this function "returns" again with 1.
int kernel_ctx_save(void);

// Restores the context captured by kernel_ctx_save() and jumps back to it.
// Never returns to its caller.
void kernel_ctx_restore(void);

#endif

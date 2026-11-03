#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

// Runs the PS/2 controller + mouse init sequence (enable aux port,
// defaults, enable data reporting) and unmasks IRQ12. Call after
// pic_init() and fb_init() -- cursor position is clamped against the
// framebuffer's pixel dimensions, so fb must be up first.
void mouse_init(void);

// IRQ12 handler -- called from irq_handler() in idt.c. Accumulates the
// 3-byte PS/2 packet and updates the tracked cursor position/buttons
// once a full packet arrives.
void mouse_handle_irq(void);

// Current absolute cursor position (clamped to [0, fb_width/height)),
// and current button state (bit0=left, bit1=right, bit2=middle).
void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons);

#endif

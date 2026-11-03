#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_handle_irq(void);
char keyboard_read_char(void);

#endif

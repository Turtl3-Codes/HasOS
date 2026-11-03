#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>

void serial_init(void);
void serial_putchar(char c);
void serial_print(const char *str);

#endif

#include "keyboard.h"
#include "serial.h"
#include <stdint.h>

#define KBD_BUFFER_SIZE 256

static char kbd_buffer[KBD_BUFFER_SIZE];
static uint16_t kbd_head = 0;
static uint16_t kbd_tail = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// Basic US QWERTY Scancode Map (Set 1)
static const char scancode_map[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,
  '*',   0, ' '
};

void keyboard_handle_irq(void) {
    uint8_t scancode = inb(0x60);

    // Ignore key release events (scancodes >= 0x80)
    if (!(scancode & 0x80)) {
        char c = scancode_map[scancode];
        if (c != 0) {
            uint16_t next_head = (kbd_head + 1) % KBD_BUFFER_SIZE;
            if (next_head != kbd_tail) {
                kbd_buffer[kbd_head] = c;
                kbd_head = next_head;
            }
        }
    }
}

char keyboard_read_char(void) {
    if (kbd_head == kbd_tail) {
        return 0; // Buffer empty
    }

    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

void keyboard_init(void) {
    kbd_head = 0;
    kbd_tail = 0;
    serial_print("PS/2 Keyboard driver ready\n");
}

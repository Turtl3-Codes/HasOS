#include "terminal.h"
#include "fb.h"
#include "shell.h"
#include "keyboard.h"

void terminal_run(void) {
    fb_text_init(0, 255, 0, 0, 0, 0);
    fb_print("Terminal -- press Esc to return to the desktop.\n");
    shell_init();

    for (;;) {
        char c = keyboard_read_char();
        if (c == 0) {
            __asm__ volatile ("hlt");
            continue;
        }
        if (c == 27) { // Esc
            break;
        }
        shell_handle_char(c);
    }
}

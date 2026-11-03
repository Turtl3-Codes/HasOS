#include "serial.h"
#include "fb.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "keyboard.h"
#include "mouse.h"
#include "shell.h"
#include "tcp.h"
#include "vfs.h"
#include "virtio.h"
#include "eth.h"
#include "limine.h"
#include <stdint.h>
#include <stddef.h>

static int fb_ready = 0;

// Diagnostic logging goes to serial only during boot -- the framebuffer
// stays dedicated to the logo + progress indicator until init finishes,
// for a splash-screen feel instead of a scrolling wall of boot text.
static void diag(const char *s) {
    serial_print(s);
}

static void fb_only_char(char c) {
    if (fb_ready) {
        fb_putchar(c);
    }
}

// Busy-waits using real PIT ticks (timer_init() + sti must have run
// already). At 100Hz, 'ticks' of 100 = 1 second.
static void delay_ticks(uint64_t ticks_to_wait) {
    uint64_t start = timer_get_ticks();
    while (timer_get_ticks() - start < ticks_to_wait) {
        __asm__ volatile ("hlt");
    }
}

// 5x7 block-letter ASCII font, just enough glyphs to spell "HASOS".
#define GLYPH_ROWS 7
#define GLYPH_COLS 5

static const char *glyph_h[GLYPH_ROWS] = {
    "#...#",
    "#...#",
    "#...#",
    "#####",
    "#...#",
    "#...#",
    "#...#",
};

static const char *glyph_a[GLYPH_ROWS] = {
    ".###.",
    "#...#",
    "#...#",
    "#####",
    "#...#",
    "#...#",
    "#...#",
};

static const char *glyph_s[GLYPH_ROWS] = {
    ".####",
    "#....",
    "#....",
    ".###.",
    "....#",
    "....#",
    "####.",
};

static const char *glyph_o[GLYPH_ROWS] = {
    ".###.",
    "#...#",
    "#...#",
    "#...#",
    "#...#",
    "#...#",
    ".###.",
};

// Draws "HASOS" centered on the current framebuffer text grid. Returns
// the row just below the logo, for positioning the progress indicator.
static uint64_t banner_print_centered(void) {
    const char **letters[5] = { glyph_h, glyph_a, glyph_s, glyph_o, glyph_s };
    const int num_letters = 5;
    const int banner_width = num_letters * GLYPH_COLS + (num_letters - 1);

    uint64_t max_cols = fb_get_max_cols();
    uint64_t max_rows = fb_get_max_rows();

    uint64_t col_pad = 0;
    if (max_cols > (uint64_t)banner_width) {
        col_pad = (max_cols - (uint64_t)banner_width) / 2;
    }

    uint64_t block_height = GLYPH_ROWS + 2;
    uint64_t row_start = 0;
    if (max_rows > block_height) {
        row_start = (max_rows - block_height) / 2;
    }

    for (int row = 0; row < GLYPH_ROWS; row++) {
        fb_set_cursor(col_pad, row_start + (uint64_t)row);
        for (int letter = 0; letter < num_letters; letter++) {
            const char *glyph_row = letters[letter][row];
            for (int col = 0; col < GLYPH_COLS; col++) {
                fb_only_char((glyph_row[col] == '#') ? '#' : ' ');
            }
            if (letter < num_letters - 1) {
                fb_only_char(' ');
            }
        }
    }

    return row_start + GLYPH_ROWS + 1;
}

void kernel_main(void) {
    serial_init();
    serial_print("\n=== HASOS Booting in 64-bit Long Mode ===\n");

    if (LIMINE_BASE_REVISION_SUPPORTED) {
        serial_print("[OK] Limine base revision 3 supported\n");
    } else {
        serial_print("[FATAL] Limine base revision 3 NOT supported by this bootloader!\n");
    }

    if (fb_init() == 0) {
        fb_ready = 1;
        fb_text_init(0, 255, 0, 0, 0, 0);
        serial_print("[OK] Framebuffer initialized\n");
    } else {
        serial_print("[WARN] No usable framebuffer found\n");
    }

    diag("=== HASOS Booting in 64-bit Long Mode ===\n");

    // GDT/IDT/PIC/timer need to be up before we can enable interrupts
    // and use the PIT for a real timed splash delay.
    gdt_init();
    idt_init();
    pic_init();
    timer_init(100); // 100 Hz -- 100 ticks = 1 second

    __asm__ volatile ("sti");
    diag("[OK] Hardware Interrupts Enabled (STI)\n");

    uint64_t progress_row = 0;
    uint64_t progress_col = 0;

    if (fb_ready) {
        progress_row = banner_print_centered();
        uint64_t max_cols = fb_get_max_cols();
        progress_col = (max_cols > 29) ? (max_cols - 29) / 2 + 10 : 0;
        fb_set_cursor(progress_col, progress_row);
    }

    // Real subsystem init -- fast, but the splash stays up regardless
    // via the delay loop below, same as the dots being cosmetic.
    pmm_init();
    vmm_init();
    fb_backbuffer_init();
    keyboard_init();
    mouse_init();
    vfs_init();
    virtio_net_init();
    eth_init();
    tcp_init();
    diag("System Running Safely with Physical Memory Manager!\n");

    // Hold the splash for ~1.5 real seconds, printing a dot every
    // ~150ms (15 ticks at 100Hz) so it doesn't look frozen.
    for (int i = 0; i < 10; i++) {
        delay_ticks(15);
        fb_only_char('.');
    }

    // Init finished -- clear the splash screen and hand off to the
    // real shell, same look as Windows' logo-then-desktop transition.
    if (fb_ready) {
        fb_text_init(0, 255, 0, 0, 0, 0);
    }

    shell_set_fb_enabled(fb_ready);
    shell_init();

    while (1) {
        char c = keyboard_read_char();
        if (c != 0) {
            shell_handle_char(c);
        }

        int32_t mouse_x, mouse_y;
        uint8_t mouse_buttons;
        mouse_get_state(&mouse_x, &mouse_y, &mouse_buttons);
        fb_cursor_tick(mouse_x, mouse_y, mouse_buttons);

        __asm__ volatile ("hlt");
    }
}

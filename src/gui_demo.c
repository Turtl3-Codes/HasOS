#include "gui_demo.h"
#include "fb.h"
#include "mouse.h"
#include "keyboard.h"
#include <stdint.h>

#define BOX_W 140
#define BOX_H 90

// Desktop background -- shared by gui_box_demo()'s initial fb_clear()
// and erase_box() below, since erasing the box just means repainting
// it with the background color.
#define DESK_R 30
#define DESK_G 30
#define DESK_B 45

static void draw_box(int32_t x, int32_t y) {
    fb_fill_rect((uint64_t)x, (uint64_t)y, BOX_W, BOX_H, 70, 130, 180);
    fb_draw_rect((uint64_t)x, (uint64_t)y, BOX_W, BOX_H, 20, 40, 70);
    fb_present_rect((uint64_t)x, (uint64_t)y, BOX_W, BOX_H);
}

static void erase_box(int32_t x, int32_t y) {
    fb_fill_rect((uint64_t)x, (uint64_t)y, BOX_W, BOX_H, DESK_R, DESK_G, DESK_B);
    fb_present_rect((uint64_t)x, (uint64_t)y, BOX_W, BOX_H);
}

void gui_box_demo(void) {
    fb_clear(DESK_R, DESK_G, DESK_B);

    int32_t box_x = 200;
    int32_t box_y = 150;
    draw_box(box_x, box_y);

    int      dragging    = 0;
    int32_t  drag_off_x  = 0;
    int32_t  drag_off_y  = 0;
    uint8_t  prev_buttons = 0;

    uint64_t max_x = fb_get_width_px()  - BOX_W;
    uint64_t max_y = fb_get_height_px() - BOX_H;

    for (;;) {
        char key = keyboard_read_char();
        if (key == 27) { // Esc
            break;
        }

        int32_t mx, my;
        uint8_t buttons;
        mouse_get_state(&mx, &my, &buttons);

        int left_now  = buttons     & 0x01;
        int left_prev = prev_buttons & 0x01;

        if (left_now && !left_prev) {
            // Fresh press -- start a drag only if it landed inside the box.
            if (mx >= box_x && mx < box_x + BOX_W &&
                my >= box_y && my < box_y + BOX_H) {
                dragging   = 1;
                drag_off_x = mx - box_x;
                drag_off_y = my - box_y;
            }
        } else if (!left_now) {
            dragging = 0;
        }

        if (dragging) {
            int32_t new_x = mx - drag_off_x;
            int32_t new_y = my - drag_off_y;
            if (new_x < 0) new_x = 0;
            if (new_y < 0) new_y = 0;
            if ((uint64_t)new_x > max_x) new_x = (int32_t)max_x;
            if ((uint64_t)new_y > max_y) new_y = (int32_t)max_y;

            if (new_x != box_x || new_y != box_y) {
                erase_box(box_x, box_y);
                box_x = new_x;
                box_y = new_y;
                draw_box(box_x, box_y);
            }
        }

        prev_buttons = buttons;

        fb_cursor_tick(mx, my, buttons);

        __asm__ volatile ("hlt");
    }

    fb_text_init(0, 255, 0, 0, 0, 0);
}

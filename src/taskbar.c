#include "taskbar.h"
#include "fb.h"
#include "wm.h"
#include "terminal.h"
#include "power.h"
#include <stdint.h>

#define START_BTN_W 70
#define MENU_ITEM_H 24
#define MENU_W      140
#define MENU_ITEM_COUNT 5

static const char *menu_items[MENU_ITEM_COUNT] = {
    "Terminal", "My Computer", "Text Editor", "Restart", "Shutdown"
};

static int      menu_open    = 0;
static uint8_t  prev_buttons = 0;

uint64_t taskbar_desktop_height(void) {
    uint64_t h = fb_get_height_px();
    return (h > TASKBAR_H) ? h - TASKBAR_H : 0;
}

static int32_t taskbar_y(void) {
    return (int32_t)taskbar_desktop_height();
}

void taskbar_draw(void) {
    uint64_t w  = fb_get_width_px();
    int32_t  ty = taskbar_y();

    fb_fill_rect(0, (uint64_t)ty, w, TASKBAR_H, 35, 35, 50);
    fb_fill_rect(4, (uint64_t)ty + 3, START_BTN_W, TASKBAR_H - 6, 60, 90, 60);
    fb_draw_text(12, (uint64_t)ty + 9, "Start", 255, 255, 255);

    uint64_t top = (uint64_t)ty;
    uint64_t present_h = TASKBAR_H;

    if (menu_open) {
        int32_t menu_y = ty - (int32_t)(MENU_ITEM_COUNT * MENU_ITEM_H);
        fb_fill_rect(4, (uint64_t)menu_y, MENU_W, MENU_ITEM_COUNT * MENU_ITEM_H, 45, 45, 65);
        fb_draw_rect(4, (uint64_t)menu_y, MENU_W, MENU_ITEM_COUNT * MENU_ITEM_H, 15, 15, 25);
        for (int i = 0; i < MENU_ITEM_COUNT; i++) {
            fb_draw_text(12, (uint64_t)menu_y + (uint64_t)i * MENU_ITEM_H + 8,
                         menu_items[i], 255, 255, 255);
        }
        top = (uint64_t)menu_y;
        present_h = TASKBAR_H + (uint64_t)(MENU_ITEM_COUNT * MENU_ITEM_H);
    }

    fb_present_rect(0, top, w, present_h);
}

int taskbar_handle_mouse(int32_t mx, int32_t my, uint8_t buttons) {
    int left_now    = buttons      & 0x01;
    int left_prev   = prev_buttons & 0x01;
    int fresh_press = left_now && !left_prev;
    prev_buttons = buttons;

    int32_t ty = taskbar_y();

    if (menu_open) {
        if (fresh_press) {
            int32_t menu_y = ty - (int32_t)(MENU_ITEM_COUNT * MENU_ITEM_H);
            if (mx >= 4 && mx < 4 + MENU_W && my >= menu_y && my < ty) {
                int idx = (my - menu_y) / MENU_ITEM_H;
                menu_open = 0;
                if (idx == 0) {
                    terminal_run();
                    wm_redraw();
                    taskbar_draw();
                } else if (idx == 1) {
                    wm_open_my_computer();
                    taskbar_draw();
                } else if (idx == 2) {
                    wm_open_text_editor();
                    taskbar_draw();
                } else if (idx == 3) {
                    hasos_restart();
                } else {
                    hasos_shutdown();
                }
            } else {
                menu_open = 0;
                wm_redraw();
            }
        }
        return 1; // menu owns all input until it's closed
    }

    if (fresh_press && my >= ty) {
        if (mx >= 4 && mx < 4 + START_BTN_W) {
            menu_open = 1;
            taskbar_draw();
        }
        return 1;
    }

    return (my >= ty) ? 1 : 0;
}

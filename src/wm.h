#ifndef WM_H
#define WM_H

#include <stdint.h>

// Creates a window and returns its id (raised to the top of z-order),
// or -1 if the window table is full. title is copied (max 31 chars).
int wm_create_window(const char *title, int32_t x, int32_t y,
                      uint32_t w, uint32_t h,
                      uint8_t r, uint8_t g, uint8_t b);

// Redraws the desktop background and every window in z-order, then
// presents the whole screen. Simple compositor -- no damage tracking
// yet, just a full redraw whenever something changes.
void wm_redraw(void);

// Feeds current mouse state to the window manager: raises the topmost
// window under the cursor on a fresh left-click, starts a drag if the
// click landed in a title bar, and moves the dragged window each tick
// its position changes. Call once per main-loop iteration.
void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons);

// Routes a typed character to the focused (topmost) window's text
// buffer and redraws it. Call once per key from the main loop.
void wm_handle_key(char c);

// Opens a window listing the ext2 root directory (read-only).
void wm_open_my_computer(void);

// Opens the next saved note, or starts a new note if none remains.
// The editor saves to ext2 through its title-bar Save button.
void wm_open_text_editor(void);

// Clears all windows and manager state back to empty.
void wm_reset(void);

// Creates a small set of demo windows and runs the desktop event loop
// (mouse-driven drag/focus, Esc to exit) until Esc is pressed, then
// restores the text console. Self-contained, like gui_box_demo().
void wm_demo_run(void);

#endif

#ifndef TASKBAR_H_INCLUDED
#define TASKBAR_H_INCLUDED

#include <stdint.h>

#define TASKBAR_H 28

// Height of the desktop area above the taskbar -- windows and the
// window-manager compositor should never draw or clamp positions past
// this, so they never overlap the taskbar strip.
uint64_t taskbar_desktop_height(void);

// Draws the taskbar (Start button, plus the open menu if any) and
// presents exactly the rows it touched. Called once at boot and again
// whenever the taskbar's own appearance changes (menu open, or after
// something else -- like the terminal -- has clobbered the screen).
void taskbar_draw(void);

// Feeds mouse state to the taskbar/Start menu. Returns 1 if the click
// was consumed (the caller should NOT also forward it to the window
// manager that tick), 0 otherwise. While the menu is open, all mouse
// activity is consumed so windows underneath don't react to it.
int taskbar_handle_mouse(int32_t mx, int32_t my, uint8_t buttons);

#endif

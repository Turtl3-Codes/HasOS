#ifndef FB_H
#define FB_H

#include <stdint.h>
#include <stddef.h>

int  fb_init(void);
void fb_clear(uint8_t r, uint8_t g, uint8_t b);
void fb_text_init(uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                   uint8_t bg_r, uint8_t bg_g, uint8_t bg_b);
void fb_putchar(char c);
void fb_print(const char *str);

// Current text-mode grid dimensions (fb_width/height divided by glyph
// size). Both return 0 if fb_init() hasn't succeeded yet.
uint64_t fb_get_max_cols(void);
uint64_t fb_get_max_rows(void);

// Raw framebuffer pixel dimensions (as opposed to the text-grid
// col/row counts above). Both return 0 if fb_init() hasn't succeeded.
uint64_t fb_get_width_px(void);
uint64_t fb_get_height_px(void);

// Moves the text cursor to an arbitrary (col, row) cell without
// printing or clearing anything. Used to position content (e.g. a
// centered boot logo) instead of always writing from the top-left.
void fb_set_cursor(uint64_t col, uint64_t row);

// Allocates and maps a virtually-contiguous back buffer sized to the
// framebuffer, and copies whatever's currently on screen into it.
// Must be called once PMM+VMM are up (fb_init() itself runs before
// that, so this is a separate step). Until this runs, all drawing
// goes straight to the real framebuffer as before; calling it later
// switches every fb_* draw call over to the back buffer transparently.
void fb_backbuffer_init(void);

// Blits the back buffer (if active) to the real framebuffer in one
// shot. Call once per frame after a batch of drawing to avoid
// tearing/flicker -- fb_putchar()/fb_clear() already call this
// internally, so existing text-console output needs no changes.
void fb_present(void);

// Blits just a w x h rectangle from the back buffer to the real
// framebuffer -- cheaper than fb_present() when only part of the
// screen changed (a moved window, a redrawn widget).
void fb_present_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h);

// Draws into the back buffer without presenting -- batch your draws,
// then call fb_present_rect()/fb_present() once.
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                   uint8_t r, uint8_t g, uint8_t b);
void fb_draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                   uint8_t r, uint8_t g, uint8_t b);

// Draws text at an arbitrary pixel position, background transparent
// (only glyph 'on' pixels are drawn). For window chrome/labels --
// fb_putchar() stays the grid-locked terminal path.
void fb_draw_text(uint64_t x, uint64_t y, const char *str,
                   uint8_t r, uint8_t g, uint8_t b);

// Draws/moves the software mouse cursor overlay. Call once per main-
// loop iteration with mouse_get_state()'s output. Safe to call before
// fb_backbuffer_init() runs or before fb is ready -- it's a no-op
// until both are up.
void fb_cursor_tick(int32_t x, int32_t y, uint8_t buttons);

#endif

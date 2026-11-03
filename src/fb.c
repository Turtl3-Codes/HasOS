#include "fb.h"
#include "limine.h"
#include "font8x8.h"
#include "pmm.h"
#include "vmm.h"

static void      *fb_addr   = NULL;
static uint64_t   fb_width  = 0;
static uint64_t   fb_height = 0;
static uint64_t   fb_pitch  = 0;
static uint16_t   fb_bpp    = 0;
static uint8_t     r_mask_shift = 0;
static uint8_t     g_mask_shift = 0;
static uint8_t     b_mask_shift = 0;

// Virtually-contiguous back buffer -- NULL until fb_backbuffer_init()
// runs (after PMM/VMM are up). While NULL, every draw call below
// falls back to writing straight to fb_addr, so boot-splash drawing
// (which happens before PMM/VMM init) is unaffected.
static uint8_t *back_buffer = NULL;
#define FB_BACKBUFFER_VBASE 0xFFFF900000000000UL

#define GLYPH_W 8
#define GLYPH_H 8

static uint32_t text_fg = 0;
static uint32_t text_bg = 0;
static uint64_t cursor_col = 0;
static uint64_t cursor_row = 0;
static uint64_t max_cols = 0;
static uint64_t max_rows = 0;

int fb_init(void) {
    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        return -1;
    }

    struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];

    fb_addr   = fb->address;
    fb_width  = fb->width;
    fb_height = fb->height;
    fb_pitch  = fb->pitch;
    fb_bpp    = fb->bpp;
    r_mask_shift = fb->red_mask_shift;
    g_mask_shift = fb->green_mask_shift;
    b_mask_shift = fb->blue_mask_shift;

    if (fb_addr == NULL || fb_bpp != 32) {
        return -1;
    }

    max_cols = fb_width / GLYPH_W;
    max_rows = fb_height / GLYPH_H;

    return 0;
}

// The build has no optimization flags and no memcpy/memset (freestanding,
// -nostdlib -- see mem_stubs.c), so plain C copy loops here run as
// unoptimized scalar loads/stores, which is what made scrolling and
// presenting slow. 'rep movsl'/'rep stosl' move a dword per iteration
// in a single hardware-microcoded instruction regardless of -O level.
static inline void fast_copy_words(uint32_t *dst, const uint32_t *src, uint64_t count) {
    __asm__ volatile (
        "rep movsl"
        : "+D"(dst), "+S"(src), "+c"(count)
        :
        : "memory"
    );
}

static inline void fast_set_words(uint32_t *dst, uint32_t value, uint64_t count) {
    __asm__ volatile (
        "rep stosl"
        : "+D"(dst), "+c"(count)
        : "a"(value)
        : "memory"
    );
}

static inline uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << r_mask_shift) |
           ((uint32_t)g << g_mask_shift) |
           ((uint32_t)b << b_mask_shift);
}

static inline void fb_put_pixel(uint64_t x, uint64_t y, uint32_t color) {
    uint8_t *target = back_buffer ? back_buffer : (uint8_t *)fb_addr;
    uint8_t *row = target + (y * fb_pitch);
    uint32_t *pixel = (uint32_t *)row;
    pixel[x] = color;
}

void fb_clear(uint8_t r, uint8_t g, uint8_t b) {
    if (fb_addr == NULL) {
        return;
    }

    uint32_t color = fb_pack_color(r, g, b);
    uint8_t *target = back_buffer ? back_buffer : (uint8_t *)fb_addr;
    uint8_t *row = target;

    for (uint64_t y = 0; y < fb_height; y++) {
        fast_set_words((uint32_t *)row, color, fb_width);
        row += fb_pitch;
    }

    fb_present();
}

// Pixel-level primitives -- unlike fb_clear()/fb_putchar(), these
// draw into the back buffer (or fb_addr directly, pre-backbuffer)
// but do NOT present. Callers batch several draws, then present just
// the affected rect(s) via fb_present_rect() (or fb_present() for a
// full-screen change) once the batch is done.
void fb_fill_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                   uint8_t r, uint8_t g, uint8_t b) {
    if (fb_addr == NULL) {
        return;
    }
    if (x >= fb_width || y >= fb_height) {
        return;
    }
    if (x + w > fb_width)  w = fb_width  - x;
    if (y + h > fb_height) h = fb_height - y;

    uint32_t color = fb_pack_color(r, g, b);
    uint8_t *target = back_buffer ? back_buffer : (uint8_t *)fb_addr;

    for (uint64_t row = 0; row < h; row++) {
        uint32_t *dst = (uint32_t *)(target + (y + row) * fb_pitch) + x;
        fast_set_words(dst, color, w);
    }
}

void fb_draw_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h,
                   uint8_t r, uint8_t g, uint8_t b) {
    if (w == 0 || h == 0) {
        return;
    }
    fb_fill_rect(x, y, w, 1, r, g, b);
    if (h > 1) {
        fb_fill_rect(x, y + h - 1, w, 1, r, g, b);
    }
    fb_fill_rect(x, y, 1, h, r, g, b);
    if (w > 1) {
        fb_fill_rect(x + w - 1, y, 1, h, r, g, b);
    }
}

// Draws text at an arbitrary pixel position (unlike fb_putchar(),
// which is locked to the 8x8 terminal grid). Only 'on' glyph pixels
// are drawn -- the background shows through -- so this works over
// any fill color, e.g. a window title bar.
void fb_draw_text(uint64_t x, uint64_t y, const char *str,
                   uint8_t r, uint8_t g, uint8_t b) {
    if (fb_addr == NULL) {
        return;
    }

    uint32_t color = fb_pack_color(r, g, b);
    uint64_t cx = x;

    for (const char *p = str; *p != '\0'; p++) {
        char c = *p;
        if ((unsigned char)c > 0x7F) {
            c = '?';
        }
        const uint8_t *glyph = font8x8_basic[(unsigned char)c];

        for (uint64_t gy = 0; gy < 8; gy++) {
            uint8_t bits = glyph[gy];
            for (uint64_t gx = 0; gx < 8; gx++) {
                if ((bits & (1 << gx)) && cx + gx < fb_width && y + gy < fb_height) {
                    fb_put_pixel(cx + gx, y + gy, color);
                }
            }
        }
        cx += 8;
    }
}

void fb_text_init(uint8_t fg_r, uint8_t fg_g, uint8_t fg_b,
                   uint8_t bg_r, uint8_t bg_g, uint8_t bg_b) {
    text_fg = fb_pack_color(fg_r, fg_g, fg_b);
    text_bg = fb_pack_color(bg_r, bg_g, bg_b);
    cursor_col = 0;
    cursor_row = 0;
    fb_clear(bg_r, bg_g, bg_b);
}

// Blits just a w x h rectangle from the back buffer to the real
// framebuffer, instead of the whole screen. fb_draw_glyph() uses
// this so typing a single character only costs an 8x8-pixel copy
// instead of a full-framebuffer memcpy -- fb_present() (the
// full-screen version) stays reserved for scrolls and fb_clear().
void fb_present_rect(uint64_t x, uint64_t y, uint64_t w, uint64_t h) {
    if (back_buffer == NULL || fb_addr == NULL) {
        return;
    }
    if (x >= fb_width || y >= fb_height) {
        return;
    }
    if (x + w > fb_width)  w = fb_width  - x;
    if (y + h > fb_height) h = fb_height - y;

    for (uint64_t row = 0; row < h; row++) {
        uint32_t *src = (uint32_t *)(back_buffer + (y + row) * fb_pitch + x * 4);
        uint32_t *dst = (uint32_t *)((uint8_t *)fb_addr + (y + row) * fb_pitch + x * 4);
        fast_copy_words(dst, src, w);
    }
}

static void fb_draw_glyph(char c, uint64_t col, uint64_t row) {
    if ((unsigned char)c > 0x7F) {
        c = '?';
    }
    const uint8_t *glyph = font8x8_basic[(unsigned char)c];

    uint64_t base_x = col * GLYPH_W;
    uint64_t base_y = row * GLYPH_H;

    for (uint64_t y = 0; y < GLYPH_H; y++) {
        uint8_t bits = glyph[y];
        for (uint64_t x = 0; x < GLYPH_W; x++) {
            uint32_t color = (bits & (1 << x)) ? text_fg : text_bg;
            fb_put_pixel(base_x + x, base_y + y, color);
        }
    }

    fb_present_rect(base_x, base_y, GLYPH_W, GLYPH_H);
}

static void fb_copy_row(uint64_t dst_y, uint64_t src_y) {
    uint8_t *target = back_buffer ? back_buffer : (uint8_t *)fb_addr;
    uint32_t *dst = (uint32_t *)(target + dst_y * fb_pitch);
    uint32_t *src = (uint32_t *)(target + src_y * fb_pitch);
    fast_copy_words(dst, src, fb_pitch / 4);
}

static void fb_clear_row(uint64_t y, uint32_t color) {
    uint8_t *target = back_buffer ? back_buffer : (uint8_t *)fb_addr;
    uint32_t *row = (uint32_t *)(target + y * fb_pitch);
    fast_set_words(row, color, fb_width);
}

static void fb_scroll(void) {
    for (uint64_t y = 0; y < fb_height - GLYPH_H; y++) {
        fb_copy_row(y, y + GLYPH_H);
    }
    for (uint64_t y = fb_height - GLYPH_H; y < fb_height; y++) {
        fb_clear_row(y, text_bg);
    }

    fb_present();
}

static void fb_newline(void) {
    cursor_col = 0;
    cursor_row++;
    if (cursor_row >= max_rows) {
        fb_scroll();
        cursor_row = max_rows - 1;
    }
}

void fb_putchar(char c) {
    if (fb_addr == NULL) {
        return;
    }

    if (c == '\n') {
        fb_newline();
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            fb_draw_glyph(' ', cursor_col, cursor_row);
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = max_cols - 1;
            fb_draw_glyph(' ', cursor_col, cursor_row);
        }
    } else {
        fb_draw_glyph(c, cursor_col, cursor_row);
        cursor_col++;
        if (cursor_col >= max_cols) {
            fb_newline();
        }
    }
}

void fb_print(const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        fb_putchar(str[i]);
    }
}

uint64_t fb_get_width_px(void) {
    return fb_width;
}

uint64_t fb_get_height_px(void) {
    return fb_height;
}

uint64_t fb_get_max_cols(void) {
    return max_cols;
}

uint64_t fb_get_max_rows(void) {
    return max_rows;
}

void fb_set_cursor(uint64_t col, uint64_t row) {
    if (col < max_cols) {
        cursor_col = col;
    }
    if (row < max_rows) {
        cursor_row = row;
    }
}

void fb_backbuffer_init(void) {
    if (fb_addr == NULL || back_buffer != NULL) {
        return;
    }

    uint64_t size  = fb_pitch * fb_height;
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            // Out of memory -- bail out and keep drawing directly to
            // fb_addr; back_buffer stays NULL so every draw call
            // above keeps falling back to single-buffered mode.
            back_buffer = NULL;
            return;
        }
        vmm_map(FB_BACKBUFFER_VBASE + (i * PAGE_SIZE), phys, PAGE_PRESENT | PAGE_WRITE);
    }

    back_buffer = (uint8_t *)FB_BACKBUFFER_VBASE;

    // Copy whatever's already on screen (the boot splash, drawn
    // before this ran) into the new back buffer so fb_present()
    // doesn't erase it on the next flip.
    for (uint64_t i = 0; i < size; i++) {
        back_buffer[i] = ((uint8_t *)fb_addr)[i];
    }
}

void fb_present(void) {
    if (back_buffer == NULL || fb_addr == NULL) {
        return;
    }

    uint64_t words = (fb_pitch * fb_height) / 4;
    fast_copy_words((uint32_t *)fb_addr, (uint32_t *)back_buffer, words);
}

// Software mouse cursor. Drawn as an overlay directly onto the real
// framebuffer (never into back_buffer), so it never becomes part of
// the 'real' scene -- to move it, we restore the real content under
// the old position (via fb_present_rect, which reads back_buffer)
// and then draw the sprite at the new position on top.
#define CURSOR_W 10
#define CURSOR_H 16

static const uint16_t cursor_bitmap[CURSOR_H] = {
    0x0200, 0x0300, 0x0380, 0x03C0, 0x03E0, 0x03F0, 0x03F8, 0x03FC,
    0x03FE, 0x03E0, 0x03B0, 0x0330, 0x0218, 0x0018, 0x000C, 0x000C,
};

static int32_t last_cursor_x = -1;
static int32_t last_cursor_y = -1;
static int      cursor_drawn = 0;

static inline void fb_put_pixel_direct(uint64_t x, uint64_t y, uint32_t color) {
    uint32_t *row = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch);
    row[x] = color;
}

// Call once per main-loop iteration with the mouse driver's current
// state. No-ops if the position hasn't changed since the last call,
// so idle polling costs nothing.
void fb_cursor_tick(int32_t x, int32_t y, uint8_t buttons) {
    (void)buttons; // click feedback comes later, once there's something to click

    if (back_buffer == NULL || fb_addr == NULL) {
        return;
    }
    if (cursor_drawn && x == last_cursor_x && y == last_cursor_y) {
        return;
    }

    if (cursor_drawn) {
        uint64_t ew = CURSOR_W;
        uint64_t eh = CURSOR_H;
        if ((uint64_t)last_cursor_x + ew > fb_width)  ew = fb_width  - (uint64_t)last_cursor_x;
        if ((uint64_t)last_cursor_y + eh > fb_height) eh = fb_height - (uint64_t)last_cursor_y;
        fb_present_rect((uint64_t)last_cursor_x, (uint64_t)last_cursor_y, ew, eh);
    }

    uint64_t dw = CURSOR_W;
    uint64_t dh = CURSOR_H;
    if ((uint64_t)x + dw > fb_width)  dw = fb_width  - (uint64_t)x;
    if ((uint64_t)y + dh > fb_height) dh = fb_height - (uint64_t)y;

    uint32_t white = fb_pack_color(255, 255, 255);

    for (uint64_t row = 0; row < dh; row++) {
        uint16_t bits = cursor_bitmap[row];
        for (uint64_t col = 0; col < dw; col++) {
            if (bits & (1U << (CURSOR_W - 1 - col))) {
                fb_put_pixel_direct((uint64_t)x + col, (uint64_t)y + row, white);
            }
        }
    }

    last_cursor_x = x;
    last_cursor_y = y;
    cursor_drawn = 1;
}

#include "wm.h"
#include "fb.h"
#include "mouse.h"
#include "keyboard.h"
#include "taskbar.h"
#include "ext2.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *str);

#define MAX_WINDOWS 8
#define TITLE_H     20

// Close button: a small square in the title bar's top-right corner.
#define CLOSE_BTN_SIZE   14
#define CLOSE_BTN_MARGIN 3

// Save button: same size, sitting just left of the close button.
// Only drawn/hit-tested on windows with is_editor set.
#define SAVE_BTN_SIZE   14
#define SAVE_BTN_MARGIN 3

// Resize grip: a small square in the bottom-right corner.
#define RESIZE_GRIP_SIZE 10
#define MIN_WIN_W        80
#define MIN_WIN_H        (TITLE_H + 40)

// Minimal per-window content: a typed text buffer, so keyboard focus
// routing has something visible to demonstrate. Not a real text
// widget (no cursor, no scrolling past what fits) -- just enough to
// show input reaching the right window.
#define WIN_TEXT_MAX 256
#define EDITOR_STATUS_MAX 40

#define DESK_R 20
#define DESK_G 20
#define DESK_B 30

struct wm_window {
    int      active;
    int32_t  x, y;
    uint32_t w, h;
    char     title[32];
    uint8_t  r, g, b; // content area fill color
    char     text[WIN_TEXT_MAX];
    int      text_len;
    int      is_editor;
    char     filename[32];
    char     editor_status[EDITOR_STATUS_MAX];
    int      is_explorer;
};

static struct wm_window windows[MAX_WINDOWS];
static int               z_order[MAX_WINDOWS]; // bottom -> top, indices into windows[]
static int               z_count = 0;

static int      dragging_idx = -1;
static int32_t  drag_off_x   = 0;
static int32_t  drag_off_y   = 0;

static int      resizing_idx    = -1;
static int32_t  resize_start_mx = 0;
static int32_t  resize_start_my = 0;
static uint32_t resize_start_w  = 0;
static uint32_t resize_start_h  = 0;

static uint8_t  prev_buttons = 0;

void wm_reset(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        windows[i].active = 0;
    }
    z_count      = 0;
    dragging_idx = -1;
    resizing_idx = -1;
    prev_buttons = 0;
}

int wm_create_window(const char *title, int32_t x, int32_t y,
                      uint32_t w, uint32_t h,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].active) {
            continue;
        }

        windows[i].active = 1;
        windows[i].x = x;
        windows[i].y = y;
        windows[i].w = w;
        windows[i].h = h;
        windows[i].r = r;
        windows[i].g = g;
        windows[i].b = b;

        int j = 0;
        while (title[j] != '\0' && j < 31) {
            windows[i].title[j] = title[j];
            j++;
        }
        windows[i].title[j] = '\0';

        windows[i].text[0] = '\0';
        windows[i].text_len = 0;
        windows[i].is_editor = 0;
        windows[i].filename[0] = '\0';
        windows[i].editor_status[0] = '\0';
        windows[i].is_explorer = 0;

        if (z_count < MAX_WINDOWS) {
            z_order[z_count++] = i;
        }
        return i;
    }
    return -1;
}

// Moves the window at windows[idx] to the top of z_order (drawn/hit-
// tested last, i.e. on top and focused). No-op if already on top.
static void raise_window(int idx) {
    int pos = -1;
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == idx) {
            pos = i;
            break;
        }
    }
    if (pos < 0 || pos == z_count - 1) {
        return;
    }
    for (int i = pos; i < z_count - 1; i++) {
        z_order[i] = z_order[i + 1];
    }
    z_order[z_count - 1] = idx;
}

static int rects_intersect(int32_t ax, int32_t ay, int32_t aw, int32_t ah,
                            int32_t bx, int32_t by, int32_t bw, int32_t bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

// Set only while a My-Computer window's directory listing is being
// populated -- ext2_list_root()'s callback has no user-data pointer,
// so this is how it knows which window's text buffer to append to.
static struct wm_window *explorer_target = NULL;

static void explorer_dirent_cb(const char *name, uint32_t inode_num, uint8_t file_type) {
    (void)inode_num;
    if (explorer_target == NULL) {
        return;
    }
    struct wm_window *w = explorer_target;

    int j = w->text_len;
    int k = 0;
    while (name[k] != '\0' && j < WIN_TEXT_MAX - 3) {
        w->text[j++] = name[k++];
    }
    if (file_type == EXT2_FT_DIR && j < WIN_TEXT_MAX - 2) {
        w->text[j++] = '/';
    }
    if (j < WIN_TEXT_MAX - 1) {
        w->text[j++] = '\n';
    }
    w->text_len = j;
    w->text[j] = '\0';
}

// Opens a window listing the root directory of the mounted ext2
// filesystem (read-only; no subfolder navigation yet). Called from
// the taskbar's Start menu.
void wm_open_my_computer(void) {
    int idx = wm_create_window("My Computer", 260, 80, 240, 180, 25, 25, 35);
    if (idx < 0) {
        return;
    }
    struct wm_window *w = &windows[idx];
    w->is_explorer = 1;

    if (!ext2_ensure_mounted()) {
        const char *msg = "No disk found.\n";
        int j = 0;
        while (msg[j] != '\0') {
            w->text[j] = msg[j];
            j++;
        }
        w->text[j] = '\0';
        w->text_len = j;
    } else {
        explorer_target = w;
        ext2_list_root(explorer_dirent_cb);
        explorer_target = NULL;
    }

    wm_redraw();
}

// Extracts the line_index-th newline-delimited entry from an explorer
// window's text buffer into out (bounded to out_size - 1 chars).
// Returns 0 (and writes nothing) if that line doesn't exist or names
// a directory (trailing '/') -- subfolder navigation isn't supported
// yet, so those clicks are silently ignored for now.
static int explorer_line_at(struct wm_window *w, int line_index, char *out, int out_size) {
    int line = 0;
    int pos  = 0;
    while (pos < w->text_len && line < line_index) {
        if (w->text[pos] == '\n') {
            line++;
        }
        pos++;
    }
    if (pos >= w->text_len) {
        return 0;
    }
    int start = pos;
    while (pos < w->text_len && w->text[pos] != '\n') {
        pos++;
    }
    int end = pos;
    if (end > start && w->text[end - 1] == '/') {
        return 0;
    }
    int len = end - start;
    if (len <= 0 || len >= out_size) {
        return 0;
    }
    for (int i = 0; i < len; i++) {
        out[i] = w->text[start + i];
    }
    out[len] = '\0';
    return 1;
}

static void editor_set_status(struct wm_window *w, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0' && i < EDITOR_STATUS_MAX - 1) {
        w->editor_status[i] = prefix[i];
        i++;
    }
    for (size_t j = 0; w->filename[j] != '\0' && i < EDITOR_STATUS_MAX - 1; j++) {
        w->editor_status[i++] = w->filename[j];
    }
    w->editor_status[i] = '\0';
}

// Opens 'name' from the mounted ext2 filesystem into a new Text
// Editor window, so its Save button (already wired) will write back
// to the same file.
static void wm_open_file_in_editor(const char *name) {
    int idx = wm_create_window("Text Editor", 300, 100, 220, 150, 30, 30, 40);
    if (idx < 0) {
        return;
    }
    struct wm_window *w = &windows[idx];
    w->is_editor = 1;

    int fi = 0;
    while (name[fi] != '\0' && fi < 31) {
        w->filename[fi] = name[fi];
        fi++;
    }
    w->filename[fi] = '\0';

    size_t out_len = 0;
    if (!ext2_ensure_mounted()) {
        w->text[0] = '\0';
        w->text_len = 0;
        editor_set_status(w, "Disk unavailable: ");
    } else if (ext2_read_file(w->filename, (uint8_t *)w->text, WIN_TEXT_MAX - 1, &out_len)) {
        size_t text_len = out_len;
        if (text_len > WIN_TEXT_MAX - 1) {
            text_len = WIN_TEXT_MAX - 1;
        }
        w->text_len = (int)text_len;
        w->text[w->text_len] = '\0';
        editor_set_status(w, (out_len > text_len) ? "Loaded (truncated): " : "Loaded: ");
    } else {
        w->text[0] = '\0';
        w->text_len = 0;
        editor_set_status(w, "Load failed: ");
    }

    wm_redraw();
}

static uint32_t editor_counter = 0;
static uint32_t editor_saved_note = 0;

static int editor_parse_note_number(const char *name, uint32_t *out_number) {
    if (name[0] != 'n' || name[1] != 'o' || name[2] != 't' || name[3] != 'e') {
        return 0;
    }

    size_t pos = 4;
    if (name[pos] < '0' || name[pos] > '9') {
        return 0;
    }

    uint32_t number = 0;
    while (name[pos] >= '0' && name[pos] <= '9') {
        uint32_t digit = (uint32_t)(name[pos] - '0');
        if (number > (UINT32_MAX - digit) / 10) {
            return 0;
        }
        number = number * 10 + digit;
        pos++;
    }
    if (pos > 251) {
        return 0;
    }
    if (name[pos] != '.' || name[pos + 1] != 't' ||
        name[pos + 2] != 'x' || name[pos + 3] != 't' ||
        name[pos + 4] != '\0' || number == 0) {
        return 0;
    }

    *out_number = number;
    return 1;
}

static void editor_note_dirent_cb(const char *name, uint32_t inode_num, uint8_t file_type) {
    (void)inode_num;
    if (file_type != EXT2_FT_REG_FILE) {
        return;
    }

    uint32_t number;
    if (editor_parse_note_number(name, &number) &&
        number > editor_counter &&
        (editor_saved_note == 0 || number < editor_saved_note)) {
        editor_saved_note = number;
    }
}

static void editor_format_filename(uint32_t number, char *filename) {
    char digits[10];
    size_t digit_count = 0;
    do {
        digits[digit_count++] = (char)('0' + number % 10);
        number /= 10;
    } while (number > 0 && digit_count < sizeof(digits));

    size_t pos = 0;
    const char *prefix = "note";
    for (size_t i = 0; prefix[i] != '\0'; i++) {
        filename[pos++] = prefix[i];
    }
    while (digit_count > 0) {
        filename[pos++] = digits[--digit_count];
    }
    const char *suffix = ".txt";
    for (size_t i = 0; suffix[i] != '\0'; i++) {
        filename[pos++] = suffix[i];
    }
    filename[pos] = '\0';
}

void wm_open_text_editor(void) {
    int idx = wm_create_window("Text Editor", 300, 100, 220, 150, 30, 30, 40);
    if (idx < 0) {
        return;
    }
    struct wm_window *w = &windows[idx];
    w->is_editor = 1;

    int mounted = ext2_ensure_mounted();
    editor_saved_note = 0;
    int listing_ok = 0;
    if (mounted) {
        listing_ok = ext2_list_root(editor_note_dirent_cb) >= 0;
    }

    uint32_t number = editor_saved_note;
    if (number == 0) {
        if (editor_counter == UINT32_MAX) {
            editor_set_status(w, "No note slots: ");
            wm_redraw();
            return;
        }
        number = editor_counter + 1;
    }
    editor_counter = number;
    editor_format_filename(number, w->filename);

    if (!mounted) {
        w->text[0] = '\0';
        w->text_len = 0;
        editor_set_status(w, "Disk unavailable: ");
    } else if (!listing_ok) {
        w->text[0] = '\0';
        w->text_len = 0;
        editor_set_status(w, "Directory error: ");
    } else if (editor_saved_note != 0) {
        size_t out_len = 0;
        if (ext2_read_file(w->filename, (uint8_t *)w->text, WIN_TEXT_MAX - 1, &out_len)) {
            size_t text_len = out_len;
            if (text_len > WIN_TEXT_MAX - 1) {
                text_len = WIN_TEXT_MAX - 1;
            }
            w->text_len = (int)text_len;
            w->text[w->text_len] = '\0';
            editor_set_status(w, (out_len > text_len) ? "Loaded (truncated): " : "Loaded: ");
        } else {
            w->text[0] = '\0';
            w->text_len = 0;
            editor_set_status(w, "Load failed: ");
        }
    } else {
        w->text[0] = '\0';
        w->text_len = 0;
        editor_set_status(w, "New: ");
    }

    wm_redraw();
}

// Fills the background and redraws only the windows that overlap
// (rx,ry,rw,rh) into the back buffer, clamped to that rect's own
// area -- no present. A drag tick costs O(dirty area) this way,
// not O(whole screen), same idea as fb_draw_glyph's partial present
// but one level up (the compositing itself, not just the flip).
static void wm_composite_rect(int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    fb_fill_rect((uint64_t)rx, (uint64_t)ry, (uint64_t)rw, (uint64_t)rh,
                 DESK_R, DESK_G, DESK_B);

    for (int i = 0; i < z_count; i++) {
        struct wm_window *w = &windows[z_order[i]];
        if (!w->active) {
            continue;
        }
        if (!rects_intersect(w->x, w->y, (int32_t)w->w, (int32_t)w->h, rx, ry, rw, rh)) {
            continue;
        }

        fb_fill_rect((uint64_t)w->x, (uint64_t)w->y, w->w, TITLE_H, 60, 60, 90);
        fb_draw_text((uint64_t)w->x + 4, (uint64_t)w->y + 6, w->title, 255, 255, 255);

        int32_t btn_x = w->x + (int32_t)w->w - CLOSE_BTN_MARGIN - CLOSE_BTN_SIZE;
        int32_t btn_y = w->y + (TITLE_H - CLOSE_BTN_SIZE) / 2;
        fb_fill_rect((uint64_t)btn_x, (uint64_t)btn_y, CLOSE_BTN_SIZE, CLOSE_BTN_SIZE, 180, 60, 60);
        fb_draw_text((uint64_t)btn_x + (CLOSE_BTN_SIZE - 8) / 2,
                      (uint64_t)btn_y + (CLOSE_BTN_SIZE - 8) / 2, "X", 255, 255, 255);

        if (w->is_editor) {
            int32_t save_x = btn_x - SAVE_BTN_MARGIN - SAVE_BTN_SIZE;
            int32_t save_y = w->y + (TITLE_H - SAVE_BTN_SIZE) / 2;
            fb_fill_rect((uint64_t)save_x, (uint64_t)save_y, SAVE_BTN_SIZE, SAVE_BTN_SIZE, 60, 140, 60);
            fb_draw_text((uint64_t)save_x + (SAVE_BTN_SIZE - 8) / 2,
                          (uint64_t)save_y + (SAVE_BTN_SIZE - 8) / 2, "S", 255, 255, 255);
        }

        fb_fill_rect((uint64_t)w->x, (uint64_t)w->y + TITLE_H,
                      w->w, w->h - TITLE_H, w->r, w->g, w->b);

        {
            uint64_t line_h = 10;
            uint64_t status_h = w->is_editor ? 12 : 0;
            uint64_t avail_h = w->h > (TITLE_H + 4 + status_h)
                             ? w->h - TITLE_H - 4 - status_h : 0;
            uint64_t max_lines = avail_h / line_h;
            uint64_t chars_per_line = (w->w > 8) ? (w->w - 8) / 8 : 0;
            char linebuf[80];
            if (chars_per_line > sizeof(linebuf) - 1) {
                chars_per_line = sizeof(linebuf) - 1;
            }
            uint64_t pos = 0;
            for (uint64_t line = 0; line < max_lines && pos < (uint64_t)w->text_len; line++) {
                uint64_t copied = 0;
                while (copied < chars_per_line && pos < (uint64_t)w->text_len &&
                       w->text[pos] != '\n') {
                    linebuf[copied] = w->text[pos];
                    copied++;
                    pos++;
                }
                linebuf[copied] = '\0';
                fb_draw_text((uint64_t)w->x + 4, (uint64_t)w->y + TITLE_H + 4 + line * line_h,
                             linebuf, 255, 255, 255);
                if (pos < (uint64_t)w->text_len && w->text[pos] == '\n') {
                    pos++;
                }
            }
        }

        if (w->is_editor && w->h >= 12) {
            uint64_t status_y = (uint64_t)w->y + w->h - 12;
            fb_fill_rect((uint64_t)w->x, status_y, w->w, 12, 20, 20, 30);
            fb_draw_text((uint64_t)w->x + 4, status_y + 2, w->editor_status, 220, 220, 220);
        }

        fb_draw_rect((uint64_t)w->x, (uint64_t)w->y, w->w, w->h, 10, 10, 15);

        int32_t grip_x = w->x + (int32_t)w->w - RESIZE_GRIP_SIZE;
        int32_t grip_y = w->y + (int32_t)w->h - RESIZE_GRIP_SIZE;
        fb_fill_rect((uint64_t)grip_x, (uint64_t)grip_y,
                     RESIZE_GRIP_SIZE, RESIZE_GRIP_SIZE, 120, 120, 120);
    }
}

// Bounding box covering both an old and a new rect -- exactly the
// region that can have visibly changed when a window of unchanged
// size moves from one position to another.
static void union_rect(int32_t x1, int32_t y1, uint32_t w1, uint32_t h1,
                        int32_t x2, int32_t y2, uint32_t w2, uint32_t h2,
                        int32_t *out_x, int32_t *out_y, int32_t *out_w, int32_t *out_h) {
    int32_t ux = (x1 < x2) ? x1 : x2;
    int32_t uy = (y1 < y2) ? y1 : y2;
    int32_t ex = (x1 + (int32_t)w1 > x2 + (int32_t)w2) ? x1 + (int32_t)w1 : x2 + (int32_t)w2;
    int32_t ey = (y1 + (int32_t)h1 > y2 + (int32_t)h2) ? y1 + (int32_t)h1 : y2 + (int32_t)h2;
    if (ux < 0) ux = 0;
    if (uy < 0) uy = 0;
    *out_x = ux;
    *out_y = uy;
    *out_w = ex - ux;
    *out_h = ey - uy;
}

// Composites only up to the top of the taskbar strip -- wm.c never
// draws into or clamps windows past that boundary (see the drag/resize
// clamps below), so the taskbar (a separate layer owned by taskbar.c)
// is never touched or erased by anything in here.
void wm_redraw(void) {
    wm_composite_rect(0, 0, (int32_t)fb_get_width_px(), (int32_t)taskbar_desktop_height());
    fb_present();
}

static int point_in_close_button(struct wm_window *w, int32_t mx, int32_t my) {
    int32_t bx = w->x + (int32_t)w->w - CLOSE_BTN_MARGIN - CLOSE_BTN_SIZE;
    int32_t by = w->y + (TITLE_H - CLOSE_BTN_SIZE) / 2;
    return mx >= bx && mx < bx + CLOSE_BTN_SIZE &&
           my >= by && my < by + CLOSE_BTN_SIZE;
}

static int point_in_save_button(struct wm_window *w, int32_t mx, int32_t my) {
    int32_t close_bx = w->x + (int32_t)w->w - CLOSE_BTN_MARGIN - CLOSE_BTN_SIZE;
    int32_t sx = close_bx - SAVE_BTN_MARGIN - SAVE_BTN_SIZE;
    int32_t sy = w->y + (TITLE_H - SAVE_BTN_SIZE) / 2;
    return mx >= sx && mx < sx + SAVE_BTN_SIZE &&
           my >= sy && my < sy + SAVE_BTN_SIZE;
}

// Writes the window's typed text buffer to disk under its assigned
// filename, creating the file if it does not already exist. Results
// are shown in the editor status strip and logged to serial.
static void wm_save_editor_content(struct wm_window *w) {
    if (w->filename[0] == '\0') {
        serial_print("[editor] save failed: no filename\n");
        editor_set_status(w, "No filename");
        wm_composite_rect(w->x, w->y, (int32_t)w->w, (int32_t)w->h);
        fb_present_rect((uint64_t)w->x, (uint64_t)w->y, w->w, w->h);
        return;
    }
    if (!ext2_ensure_mounted()) {
        serial_print("[editor] save failed: no disk\n");
        editor_set_status(w, "Disk unavailable: ");
        wm_composite_rect(w->x, w->y, (int32_t)w->w, (int32_t)w->h);
        fb_present_rect((uint64_t)w->x, (uint64_t)w->y, w->w, w->h);
        return;
    }
    if (ext2_write_file(w->filename, (const uint8_t *)w->text, (size_t)w->text_len)) {
        serial_print("[editor] saved ");
        serial_print(w->filename);
        serial_print("\n");
        editor_set_status(w, "Saved: ");
    } else {
        serial_print("[editor] save failed\n");
        editor_set_status(w, "Save failed: ");
    }
    wm_composite_rect(w->x, w->y, (int32_t)w->w, (int32_t)w->h);
    fb_present_rect((uint64_t)w->x, (uint64_t)w->y, w->w, w->h);
}

static int point_in_resize_grip(struct wm_window *w, int32_t mx, int32_t my) {
    int32_t gx = w->x + (int32_t)w->w - RESIZE_GRIP_SIZE;
    int32_t gy = w->y + (int32_t)w->h - RESIZE_GRIP_SIZE;
    return mx >= gx && mx < gx + RESIZE_GRIP_SIZE &&
           my >= gy && my < gy + RESIZE_GRIP_SIZE;
}

// Deactivates the window and removes it from z_order (shifting the
// rest down), so it's no longer drawn or hit-tested. The slot in
// windows[] stays reusable for the next wm_create_window() call.
static void wm_close_window(int idx) {
    windows[idx].active = 0;

    int pos = -1;
    for (int i = 0; i < z_count; i++) {
        if (z_order[i] == idx) {
            pos = i;
            break;
        }
    }
    if (pos >= 0) {
        for (int i = pos; i < z_count - 1; i++) {
            z_order[i] = z_order[i + 1];
        }
        z_count--;
    }

    if (dragging_idx == idx) {
        dragging_idx = -1;
    }
    if (resizing_idx == idx) {
        resizing_idx = -1;
    }
}

// Routes a typed character to the focused window (always the
// topmost in z-order, same window raise_window() maintains for
// mouse focus) and redraws just that window. Call once per key from
// the main loop; Esc should be handled by the caller before this,
// same as it already is for exiting the desktop.
void wm_handle_key(char c) {
    if (z_count == 0) {
        return;
    }
    int idx = z_order[z_count - 1];
    struct wm_window *w = &windows[idx];
    if (!w->active) {
        return;
    }

    if (c == '\b') {
        if (w->text_len > 0) {
            w->text_len--;
            w->text[w->text_len] = '\0';
        }
    } else if ((c >= 32 && c < 127) || c == '\n') {
        if (w->text_len < WIN_TEXT_MAX - 1) {
            w->text[w->text_len++] = c;
            w->text[w->text_len] = '\0';
        }
    } else {
        return; // ignore other control characters
    }

    if (w->is_editor) {
        editor_set_status(w, "Unsaved: ");
    }
    wm_composite_rect(w->x, w->y, (int32_t)w->w, (int32_t)w->h);
    fb_present_rect((uint64_t)w->x, (uint64_t)w->y, w->w, w->h);
}

void wm_handle_mouse(int32_t mx, int32_t my, uint8_t buttons) {
    int left_now  = buttons      & 0x01;
    int left_prev = prev_buttons & 0x01;

    if (left_now && !left_prev) {
        int hit = -1;
        for (int i = z_count - 1; i >= 0; i--) {
            struct wm_window *w = &windows[z_order[i]];
            if (mx >= w->x && mx < w->x + (int32_t)w->w &&
                my >= w->y && my < w->y + (int32_t)w->h) {
                hit = z_order[i];
                break;
            }
        }

        if (hit >= 0) {
            struct wm_window *w = &windows[hit];

            if (w->is_editor && point_in_save_button(w, mx, my)) {
                wm_save_editor_content(w);
                prev_buttons = buttons;
                return;
            }

            if (point_in_close_button(w, mx, my)) {
                wm_close_window(hit);
                wm_redraw();
                prev_buttons = buttons;
                return;
            }

            if (point_in_resize_grip(w, mx, my)) {
                int was_top_r = (z_order[z_count - 1] == hit);
                raise_window(hit);
                if (!was_top_r) {
                    wm_redraw();
                }
                resizing_idx    = hit;
                resize_start_mx = mx;
                resize_start_my = my;
                resize_start_w  = w->w;
                resize_start_h  = w->h;
                prev_buttons = buttons;
                return;
            }

            int was_top = (z_order[z_count - 1] == hit);
            raise_window(hit);
            if (!was_top) {
                wm_redraw();
            }

            if (my < w->y + TITLE_H) {
                dragging_idx = hit;
                drag_off_x   = mx - w->x;
                drag_off_y   = my - w->y;
            } else if (w->is_explorer) {
                int32_t content_top = w->y + TITLE_H + 4;
                if (my >= content_top) {
                    int  line_h       = 10;
                    int  clicked_line = (my - content_top) / line_h;
                    char fname[32];
                    if (explorer_line_at(w, clicked_line, fname, (int)sizeof(fname))) {
                        wm_open_file_in_editor(fname);
                    }
                }
            }
        }
    } else if (!left_now) {
        dragging_idx = -1;
        resizing_idx = -1;
    }

    if (dragging_idx >= 0) {
        struct wm_window *w = &windows[dragging_idx];

        int32_t new_x = mx - drag_off_x;
        int32_t new_y = my - drag_off_y;

        uint64_t max_x = fb_get_width_px()  - w->w;
        uint64_t max_y = taskbar_desktop_height() - w->h;
        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if ((uint64_t)new_x > max_x) new_x = (int32_t)max_x;
        if ((uint64_t)new_y > max_y) new_y = (int32_t)max_y;

        if (new_x != w->x || new_y != w->y) {
            int32_t old_x = w->x;
            int32_t old_y = w->y;
            w->x = new_x;
            w->y = new_y;

            int32_t rx, ry, rw, rh;
            union_rect(old_x, old_y, w->w, w->h, new_x, new_y, w->w, w->h,
                       &rx, &ry, &rw, &rh);
            wm_composite_rect(rx, ry, rw, rh);
            fb_present_rect((uint64_t)rx, (uint64_t)ry, (uint64_t)rw, (uint64_t)rh);
        }
    }

    if (resizing_idx >= 0) {
        struct wm_window *w = &windows[resizing_idx];

        int32_t new_w = (int32_t)resize_start_w + (mx - resize_start_mx);
        int32_t new_h = (int32_t)resize_start_h + (my - resize_start_my);

        if (new_w < MIN_WIN_W) new_w = MIN_WIN_W;
        if (new_h < MIN_WIN_H) new_h = MIN_WIN_H;

        uint64_t max_w = fb_get_width_px()  - (uint64_t)w->x;
        uint64_t max_h = taskbar_desktop_height() - (uint64_t)w->y;
        if ((uint64_t)new_w > max_w) new_w = (int32_t)max_w;
        if ((uint64_t)new_h > max_h) new_h = (int32_t)max_h;

        if ((uint32_t)new_w != w->w || (uint32_t)new_h != w->h) {
            uint32_t old_w = w->w;
            uint32_t old_h = w->h;
            w->w = (uint32_t)new_w;
            w->h = (uint32_t)new_h;

            int32_t rx, ry, rw, rh;
            union_rect(w->x, w->y, old_w, old_h, w->x, w->y, w->w, w->h,
                       &rx, &ry, &rw, &rh);
            wm_composite_rect(rx, ry, rw, rh);
            fb_present_rect((uint64_t)rx, (uint64_t)ry, (uint64_t)rw, (uint64_t)rh);
        }
    }

    prev_buttons = buttons;
}

void wm_demo_run(void) {
    wm_reset();
    wm_create_window("Terminal", 80,  60,  220, 150, 40,  40,  40);
    wm_create_window("Notes",    250, 120, 180, 130, 200, 200, 160);
    wm_create_window("About",    150, 220, 200, 100, 80,  120, 160);
    wm_redraw();
    taskbar_draw();

    for (;;) {
        char key = keyboard_read_char();
        if (key == 27) { // Esc
            break;
        }
        if (key != 0) {
            wm_handle_key(key);
        }

        int32_t mx, my;
        uint8_t buttons;
        mouse_get_state(&mx, &my, &buttons);

        if (!taskbar_handle_mouse(mx, my, buttons)) {
            wm_handle_mouse(mx, my, buttons);
        }
        fb_cursor_tick(mx, my, buttons);

        __asm__ volatile ("hlt");
    }

    fb_text_init(0, 255, 0, 0, 0, 0);
}

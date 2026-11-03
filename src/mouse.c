#include "mouse.h"
#include "serial.h"
#include <stdint.h>

extern void pic_unmask_irq(uint8_t irq);
extern uint64_t fb_get_width_px(void);
extern uint64_t fb_get_height_px(void);

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void ps2_wait_input_clear(void) {
    // Bit 1 of the status port: 1 while the input buffer (host -> device)
    // is still full. Spin until the controller is ready to accept a byte.
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) {
            return;
        }
    }
}

static void ps2_wait_output_full(void) {
    // Bit 0 of the status port: 1 once the output buffer (device -> host)
    // has a byte waiting to be read.
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) {
            return;
        }
    }
}

static void ps2_write_command(uint8_t cmd) {
    ps2_wait_input_clear();
    outb(PS2_COMMAND, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_input_clear();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_output_full();
    return inb(PS2_DATA);
}

// Sends a byte to the mouse itself (as opposed to the PS/2 controller)
// via the 0xD4 "write to aux port" controller command, then waits for
// the 0xFA ack the mouse sends back.
static void mouse_write(uint8_t data) {
    ps2_write_command(0xD4);
    ps2_write_data(data);
    ps2_read_data(); // discard ack (0xFA)
}

static uint8_t   packet[3];
static int       packet_idx = 0;

static int32_t   cursor_x = 0;
static int32_t   cursor_y = 0;
static uint8_t   cursor_buttons = 0;

void mouse_init(void) {
    cursor_x = (int32_t)(fb_get_width_px() / 2);
    cursor_y = (int32_t)(fb_get_height_px() / 2);

    // Enable the auxiliary (mouse) PS/2 port.
    ps2_write_command(0xA8);

    // Read the controller config byte, set bit 1 (enable IRQ12 on mouse
    // activity), clear bit 5 (enable the mouse clock line), write back.
    ps2_write_command(0x20);
    uint8_t config = ps2_read_data();
    config |= 0x02;
    config &= ~0x20;
    ps2_write_command(0x60);
    ps2_write_data(config);

    mouse_write(0xF6); // set defaults
    mouse_write(0xF4); // enable data reporting (start streaming packets)

    packet_idx = 0;

    pic_unmask_irq(12);

    serial_print("PS/2 mouse driver ready\n");
}

void mouse_handle_irq(void) {
    uint8_t data = inb(PS2_DATA);

    // Byte 0 of every packet always has bit 3 set. If we're not
    // expecting byte 0 and this bit is set anyway, we've lost sync
    // (e.g. a byte got dropped) -- resync by treating this as a new
    // byte 0 instead of corrupting the next packet.
    if (packet_idx == 0 && !(data & 0x08)) {
        return;
    }

    packet[packet_idx++] = data;
    if (packet_idx < 3) {
        return;
    }
    packet_idx = 0;

    uint8_t status = packet[0];

    // Overflow bits set means the reported delta is garbage -- drop
    // this packet rather than let the cursor jump.
    if (status & 0xC0) {
        return;
    }

    int32_t dx = packet[1];
    int32_t dy = packet[2];

    // Sign-extend using the sign bits carried in the status byte.
    if (status & 0x10) {
        dx -= 256;
    }
    if (status & 0x20) {
        dy -= 256;
    }

    // PS/2 reports +Y as "up"; screen space has +Y as "down".
    cursor_x += dx;
    cursor_y -= dy;

    int32_t max_x = (int32_t)fb_get_width_px() - 1;
    int32_t max_y = (int32_t)fb_get_height_px() - 1;
    if (cursor_x < 0) cursor_x = 0;
    if (cursor_y < 0) cursor_y = 0;
    if (max_x >= 0 && cursor_x > max_x) cursor_x = max_x;
    if (max_y >= 0 && cursor_y > max_y) cursor_y = max_y;

    cursor_buttons = status & 0x07;
}

void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons) {
    *x = cursor_x;
    *y = cursor_y;
    *buttons = cursor_buttons;
}

#include "gdt.h"
#include "serial.h"
#include <stdint.h>

extern void gdt_flush(uint64_t gdt_ptr);
extern void tss_flush(void);

static struct {
    struct gdt_entry null_desc;
    struct gdt_entry kernel_code;
    struct gdt_entry kernel_data;
    struct gdt_entry user_code;
    struct gdt_entry user_data;
    struct gdt_tss_entry tss_desc;
} __attribute__((packed)) gdt;

static struct gdt_ptr gp;
static struct tss sys_tss;
static uint8_t double_fault_stack[8192];

void tss_set_rsp0(uint64_t rsp) {
    sys_tss.rsp0 = rsp;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)&gdt;

    // 0x00: Null Descriptor
    gdt.null_desc = (struct gdt_entry){0, 0, 0, 0, 0, 0};

    // 0x08: Kernel Code 64-bit
    gdt.kernel_code = (struct gdt_entry){0, 0, 0, 0x9A, 0x20, 0};

    // 0x10: Kernel Data 64-bit
    gdt.kernel_data = (struct gdt_entry){0, 0, 0, 0x92, 0x00, 0};

    // 0x18: User Code (RPL 3)
    gdt.user_code = (struct gdt_entry){0, 0, 0, 0xFA, 0x20, 0};

    // 0x20: User Data (RPL 3)
    gdt.user_data = (struct gdt_entry){0, 0, 0, 0xF2, 0x00, 0};

    // Setup TSS
    uint64_t tss_base = (uint64_t)&sys_tss;
    uint32_t tss_limit = sizeof(sys_tss) - 1;

    for (uint32_t i = 0; i < sizeof(sys_tss); i++) {
        ((uint8_t *)&sys_tss)[i] = 0;
    }

    sys_tss.rsp0 = (uint64_t)double_fault_stack + sizeof(double_fault_stack);
    sys_tss.ist1 = (uint64_t)double_fault_stack + sizeof(double_fault_stack);
    sys_tss.iomap_base = sizeof(sys_tss);

    // 0x28: TSS Descriptor (System Segment 16-byte)
    gdt.tss_desc.limit_low   = tss_limit & 0xFFFF;
    gdt.tss_desc.base_low    = tss_base & 0xFFFF;
    gdt.tss_desc.base_middle = (tss_base >> 16) & 0xFF;
    gdt.tss_desc.access      = 0x89;
    gdt.tss_desc.granularity = (tss_limit >> 16) & 0x0F;
    gdt.tss_desc.base_high   = (tss_base >> 24) & 0xFF;
    gdt.tss_desc.base_upper  = (tss_base >> 32) & 0xFFFFFFFF;
    gdt.tss_desc.reserved    = 0;

    gdt_flush((uint64_t)&gp);
    tss_flush();

    serial_print("GDT and TSS re-initialized\n");
}

#include "shell.h"
static void shell_out(const char *s);
#include "serial.h"
#include "fb.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel_ctx.h"
#include "pci.h"
#include "ahci.h"
#include "ext2.h"
#include "vfs.h"
#include "tcp.h"
#include "eth.h"
#include <stdint.h>
#include <stddef.h>


extern uint64_t elf_load(const uint8_t *elf_data);
extern void enter_usermode(uint64_t entry_point, uint64_t user_rsp);
extern uint64_t pmm_alloc_page(void);
extern void process_register_stack(uint64_t vaddr, uint64_t phys);

extern uint8_t _binary_user_program_elf_start[];
extern uint8_t _binary_user_program_elf_end[];

#define USER_STACK_VADDR 0x700000UL
#define USER_STACK_SIZE  4096UL

#define SHELL_BUF_SIZE 128

#define ELF_FILE_BUF_SIZE 49152

static char shell_buf[SHELL_BUF_SIZE];
static size_t shell_len = 0;
static int fb_enabled = 0;
static uint8_t g_elf_filebuf[ELF_FILE_BUF_SIZE];

static void shell_cmd_httpd(void) {
    static const char response[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n"
        "Content-Length: 14\r\n"
        "\r\n"
        "Hello, HASOS!\n";

    // Fresh MAC identity for this listening session. This must only
    // happen here, before any connection state exists -- randomizing
    // mid-connection would break it, since the remote host's ARP cache
    // would still map our IP to the old MAC.
    eth_randomize_mac();

    tcp_listen(80, response, (uint16_t)(sizeof(response) - 1));
    shell_out("Generated a fresh MAC identity for this session (run 'showmac' to view it).\n");
    shell_out("TCP listening on port 80. curl http://10.0.2.15:80/\n");
}

void shell_set_fb_enabled(int enabled) {
    fb_enabled = enabled;
}

static void shell_out(const char *s) {
    serial_print(s);
    if (fb_enabled) {
        fb_print(s);
    }
}

static void shell_out_char(char c) {
    serial_putchar(c);
    if (fb_enabled) {
        fb_putchar(c);
    }
}

static size_t shell_strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int shell_strcmp(const char *a, const char *b) {
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
        i++;
    }
    return (unsigned char)a[i] - (unsigned char)b[i];
}

static int shell_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

static void str_copy_local(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src[i] != '\0' && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void shell_print_uint(uint64_t value) {
    char digits[21];
    int i = 0;

    if (value == 0) {
        shell_out_char('0');
        return;
    }

    while (value > 0) {
        digits[i++] = '0' + (value % 10);
        value /= 10;
    }

    while (i > 0) {
        shell_out_char(digits[--i]);
    }
}

static void shell_print_prompt(void) {
    shell_out("\n> ");
}

// Shared by both 'run' (embedded ELF) and 'runfile' (ELF loaded from
// ext2). Loads elf_data, allocates a user stack, and enters ring 3.
static void shell_launch_elf(const uint8_t *elf_data) {
    uint64_t entry = elf_load(elf_data);
    if (entry == 0) {
        shell_out("Failed to load user program (invalid ELF).\n");
        return;
    }

    uint64_t stack_phys = pmm_alloc_page();
    if (stack_phys == 0) {
        shell_out("Failed to allocate user stack page.\n");
        return;
    }
    vmm_map(USER_STACK_VADDR, stack_phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
    process_register_stack(USER_STACK_VADDR, stack_phys);

    uint64_t user_rsp = USER_STACK_VADDR + USER_STACK_SIZE;

    if (kernel_ctx_save() == 0) {
        shell_out("Entering ring 3...\n");
        enter_usermode(entry, user_rsp);
    }

    shell_out("Returned to kernel from user program.\n");
}

static void shell_cmd_run(void) {
    shell_out("\nLoading embedded user program...\n");
    shell_launch_elf(_binary_user_program_elf_start);
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static void shell_cmd_exit(void) {
    shell_out("\nWiping RAM before shutdown...\n");
    shell_out("Shutting down...\n");

    // pmm_secure_wipe() must be the last thing that touches memory --
    // no more allocations, no more shell_out() calls after this point.
    // (serial_print/fb_print write directly to hardware/MMIO without
    // going through the heap or PMM, so the two lines above are safe
    // to have run first.)
    pmm_secure_wipe();

    // QEMU's virtual ACPI PM1a control register (PIIX4 default base 0x600,
    // PM1a_CNT at offset 0x04). Writing SLP_EN|SLP_TYP for S5 causes QEMU
    // to exit. Only works under QEMU's default ACPI setup.
    outw(0x604, 0x2000);

    // Fallback if the shutdown port didn't take effect (e.g. real hardware,
    // or a QEMU machine type with a different ACPI base). We can't safely
    // call shell_out() here anymore (see note above), so just halt.
    __asm__ volatile ("cli");
    while (1) {
        __asm__ volatile ("hlt");
    }
}

static void shell_pci_print_device(const pci_device_t *dev) {
    shell_out("  ");
    shell_print_uint(dev->bus);
    shell_out(":");
    shell_print_uint(dev->device);
    shell_out(".");
    shell_print_uint(dev->function);
    shell_out("  class=");
    shell_print_uint(dev->class_code);
    shell_out(" subclass=");
    shell_print_uint(dev->subclass);
    shell_out(" progif=");
    shell_print_uint(dev->prog_if);
    shell_out(" vendor=");
    shell_print_uint(dev->vendor_id);
    shell_out(" device=");
    shell_print_uint(dev->device_id);
    shell_out("\n");
}

static void shell_cmd_lspci(void) {
    shell_out("\nPCI devices:\n");
    pci_scan(shell_pci_print_device);
}

static void shell_ahci_print_port(const ahci_port_info_t *info) {
    shell_out("  port ");
    shell_print_uint((uint64_t)info->port_number);
    shell_out(": ");
    switch (info->type) {
        case AHCI_DEV_SATA:   shell_out("SATA drive\n"); break;
        case AHCI_DEV_SATAPI: shell_out("SATAPI (optical) drive\n"); break;
        case AHCI_DEV_SEMB:   shell_out("enclosure management bridge\n"); break;
        case AHCI_DEV_PM:     shell_out("port multiplier\n"); break;
        default:              shell_out("unknown device\n"); break;
    }
}

static void shell_cmd_ahci(void) {
    shell_out("\nInitializing AHCI controller...\n");
    if (!ahci_init()) {
        shell_out("No AHCI controller found.\n");
        return;
    }
    shell_out("AHCI controller initialized. Ports with devices present:\n");
    int count = ahci_scan_ports(shell_ahci_print_port);
    if (count == 0) {
        shell_out("  (none)\n");
    }
}

static int g_diskread_sata_port = -1;

static void shell_diskread_find_sata(const ahci_port_info_t *info) {
    if (g_diskread_sata_port == -1 && info->type == AHCI_DEV_SATA) {
        g_diskread_sata_port = info->port_number;
    }
}

static void shell_print_hex_byte(uint8_t b) {
    const char *hex = "0123456789abcdef";
    shell_out_char(hex[(b >> 4) & 0xF]);
    shell_out_char(hex[b & 0xF]);
}

static void shell_cmd_diskread(void) {
    shell_out("\nInitializing AHCI controller...\n");
    if (!ahci_init()) {
        shell_out("No AHCI controller found.\n");
        return;
    }

    g_diskread_sata_port = -1;
    ahci_scan_ports(shell_diskread_find_sata);

    if (g_diskread_sata_port == -1) {
        shell_out("No SATA (non-optical) drive found.\n");
        return;
    }

    shell_out("Reading LBA 0 from port ");
    shell_print_uint((uint64_t)g_diskread_sata_port);
    shell_out("...\n");

    static uint8_t buf[512];
    if (!ahci_read_sectors(g_diskread_sata_port, 0, 1, buf)) {
        shell_out("Read failed.\n");
        return;
    }

    shell_out("First 64 bytes:\n");
    for (int i = 0; i < 64; i++) {
        shell_print_hex_byte(buf[i]);
        shell_out_char(' ');
        if ((i + 1) % 16 == 0) {
            shell_out("\n");
        }
    }
}

static void shell_cmd_diskwrite(void) {
    shell_out("\nInitializing AHCI controller...\n");
    if (!ahci_init()) {
        shell_out("No AHCI controller found.\n");
        return;
    }

    g_diskread_sata_port = -1;
    ahci_scan_ports(shell_diskread_find_sata);

    if (g_diskread_sata_port == -1) {
        shell_out("No SATA (non-optical) drive found.\n");
        return;
    }

    static uint8_t wbuf[512];
    for (int i = 0; i < 512; i++) {
        wbuf[i] = (uint8_t)i;
    }

    shell_out("Writing test pattern to LBA 0 on port ");
    shell_print_uint((uint64_t)g_diskread_sata_port);
    shell_out("...\n");

    if (!ahci_write_sectors(g_diskread_sata_port, 0, 1, wbuf)) {
        shell_out("Write failed.\n");
        return;
    }

    static uint8_t rbuf[512];
    if (!ahci_read_sectors(g_diskread_sata_port, 0, 1, rbuf)) {
        shell_out("Read-back failed.\n");
        return;
    }

    int mismatch = 0;
    for (int i = 0; i < 512; i++) {
        if (rbuf[i] != wbuf[i]) {
            mismatch = 1;
            break;
        }
    }

    if (mismatch) {
        shell_out("MISMATCH: read-back does not match what was written.\n");
    } else {
        shell_out("OK: read-back matches written pattern (all 512 bytes).\n");
    }
}

static int g_ext2_sata_port = -1;

static void shell_ext2_find_sata(const ahci_port_info_t *info) {
    if (g_ext2_sata_port == -1 && info->type == AHCI_DEV_SATA) {
        g_ext2_sata_port = info->port_number;
    }
}

static int shell_ext2_ensure_mounted(void) {
    if (!ahci_init()) {
        shell_out("No AHCI controller found.\n");
        return 0;
    }
    g_ext2_sata_port = -1;
    ahci_scan_ports(shell_ext2_find_sata);
    if (g_ext2_sata_port == -1) {
        shell_out("No SATA (non-optical) drive found.\n");
        return 0;
    }
    if (!ext2_mount(g_ext2_sata_port)) {
        shell_out("ext2 mount failed (bad magic or read error).\n");
        return 0;
    }
    return 1;
}

static void shell_ls_print(const char *name, uint32_t inode_num, uint8_t file_type) {
    (void)file_type;
    shell_out("  ");
    shell_out(name);
    shell_out("  (inode ");
    shell_print_uint((uint64_t)inode_num);
    shell_out(")\n");
}

static void shell_cmd_ls(void) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }
    shell_out("Root directory:\n");
    int count = ext2_list_root(shell_ls_print);
    if (count <= 0) {
        shell_out("  (empty or read error)\n");
    }
}

static void shell_cmd_cat(const char *filename) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }

    static uint8_t filebuf[4096];
    size_t out_len = 0;
    if (!ext2_read_file(filename, filebuf, sizeof(filebuf), &out_len)) {
        shell_out("File not found or read error.\n");
        return;
    }

    shell_out("--- ");
    shell_out(filename);
    shell_out(" (");
    shell_print_uint((uint64_t)out_len);
    shell_out(" bytes) ---\n");

    size_t to_print = out_len;
    if (to_print > sizeof(filebuf)) {
        to_print = sizeof(filebuf);
    }
    for (size_t i = 0; i < to_print; i++) {
        shell_out_char((char)filebuf[i]);
    }
    shell_out("\n");
}

static void shell_cmd_ext2write(const char *filename) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }

    const char *new_content = "Overwritten by HASOS ext2 write test!\n";
    size_t new_len = shell_strlen(new_content);

    shell_out("Writing ");
    shell_print_uint((uint64_t)new_len);
    shell_out(" bytes to ");
    shell_out(filename);
    shell_out("...\n");

    if (!ext2_write_file(filename, (const uint8_t *)new_content, new_len)) {
        shell_out("Write failed.\n");
        return;
    }

    shell_out("Write succeeded. Reading back...\n");

    static uint8_t rbuf[512];
    size_t out_len = 0;
    if (!ext2_read_file(filename, rbuf, sizeof(rbuf), &out_len)) {
        shell_out("Read-back failed.\n");
        return;
    }

    rbuf[out_len < sizeof(rbuf) ? out_len : sizeof(rbuf) - 1] = '\0';
    shell_out("--- read-back (");
    shell_print_uint((uint64_t)out_len);
    shell_out(" bytes) ---\n");
    shell_out((const char *)rbuf);
    shell_out("\n");
}

#define TREE_MAX_DEPTH   16
#define TREE_MAX_ENTRIES 64

typedef struct {
    char name[256];
    uint32_t inode_num;
    uint8_t file_type;
} tree_entry_t;

static tree_entry_t g_tree_entries[TREE_MAX_ENTRIES];
static int g_tree_entry_count;

static void tree_collect_cb(const char *name, uint32_t inode_num, uint8_t file_type) {
    if (g_tree_entry_count >= TREE_MAX_ENTRIES) {
        return;
    }
    str_copy_local(g_tree_entries[g_tree_entry_count].name, name, sizeof(g_tree_entries[0].name));
    g_tree_entries[g_tree_entry_count].inode_num = inode_num;
    g_tree_entries[g_tree_entry_count].file_type = file_type;
    g_tree_entry_count++;
}

// prefix is the accumulated "|   " / "    " continuation string for
// all ANCESTOR levels (not including this entry's own connector).
static void tree_walk(uint32_t inode_num, int depth, const char *prefix) {
    if (depth >= TREE_MAX_DEPTH) {
        shell_out(prefix);
        shell_out("... (max depth reached)\n");
        return;
    }

    // Snapshot this directory's entries into a local array BEFORE
    // recursing into any subdirectory -- ext2_list_dir/ext2_read_block
    // share one static scratch buffer (g_blockbuf in ext2.c), so a
    // recursive call would clobber the parent directory's block data
    // mid-walk if we recursed directly from inside the callback.
    g_tree_entry_count = 0;
    ext2_list_dir(inode_num, tree_collect_cb);

    // Filter out "." and ".." up front so we know the real last-index
    // for correct `-- vs |-- connector selection.
    int real_indices[TREE_MAX_ENTRIES];
    int real_count = 0;
    for (int i = 0; i < g_tree_entry_count; i++) {
        if (shell_strcmp(g_tree_entries[i].name, ".") == 0 || shell_strcmp(g_tree_entries[i].name, "..") == 0) {
            continue;
        }
        real_indices[real_count++] = i;
    }

    tree_entry_t entries_copy[TREE_MAX_ENTRIES];
    for (int i = 0; i < real_count; i++) {
        entries_copy[i] = g_tree_entries[real_indices[i]];
    }

    for (int i = 0; i < real_count; i++) {
        int is_last = (i == real_count - 1);

        shell_out(prefix);
        shell_out(is_last ? "`-- " : "|-- ");
        shell_out(entries_copy[i].name);

        if (entries_copy[i].file_type == EXT2_FT_DIR) {
            shell_out("/\n");

            char child_prefix[TREE_MAX_DEPTH * 4 + 8];
            size_t plen = shell_strlen(prefix);
            str_copy_local(child_prefix, prefix, sizeof(child_prefix));
            str_copy_local(child_prefix + plen, is_last ? "    " : "|   ", sizeof(child_prefix) - plen);

            tree_walk(entries_copy[i].inode_num, depth + 1, child_prefix);
        } else {
            shell_out("\n");
        }
    }

}

static void shell_cmd_tree(void) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }
    shell_out("/\n");
    tree_walk(2, 1, ""); // inode 2 = root
}

static void shell_cmd_ext2grow(const char *filename) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }

    // Build a ~3000 byte payload -- larger than a single 1024-byte
    // block, forcing ext2_write_file to allocate additional blocks.
    static char big_content[3000];
    size_t len = 0;
    const char *line = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ\n";
    size_t line_len = shell_strlen(line);
    while (len + line_len < sizeof(big_content)) {
        for (size_t i = 0; i < line_len; i++) {
            big_content[len + i] = line[i];
        }
        len += line_len;
    }

    shell_out("Writing ");
    shell_print_uint((uint64_t)len);
    shell_out(" bytes to ");
    shell_out(filename);
    shell_out(" (forces block allocation)...\n");

    if (!ext2_write_file(filename, (const uint8_t *)big_content, len)) {
        shell_out("Write failed.\n");
        return;
    }

    shell_out("Write succeeded. Reading back to verify size...\n");

    static uint8_t rbuf[4096];
    size_t out_len = 0;
    if (!ext2_read_file(filename, rbuf, sizeof(rbuf), &out_len)) {
        shell_out("Read-back failed.\n");
        return;
    }

    shell_out("Read back ");
    shell_print_uint((uint64_t)out_len);
    shell_out(" bytes (expected ");
    shell_print_uint((uint64_t)len);
    shell_out(").\n");

    int mismatch = (out_len != len);
    if (!mismatch) {
        for (size_t i = 0; i < len && i < sizeof(rbuf); i++) {
            if (rbuf[i] != (uint8_t)big_content[i]) {
                mismatch = 1;
                break;
            }
        }
    }

    if (mismatch) {
        shell_out("MISMATCH: content does not match what was written.\n");
    } else {
        shell_out("OK: grown file content matches exactly.\n");
    }
}

static void shell_cmd_vfscat(const char *filename) {
    shell_out("\n");

    int fd = vfs_open(filename);
    if (fd == -1) {
        shell_out("vfs_open failed.\n");
        return;
    }

    static char vbuf[512];
    int n = vfs_read(fd, vbuf, sizeof(vbuf) - 1);
    vfs_close(fd);

    if (n < 0) {
        shell_out("vfs_read failed.\n");
        return;
    }

    vbuf[n] = '\0';
    shell_out("--- ");
    shell_out(filename);
    shell_out(" (via vfs_open/vfs_read, ");
    shell_print_uint((uint64_t)n);
    shell_out(" bytes) ---\n");
    shell_out(vbuf);
    shell_out("\n");
}

static void shell_cmd_saveelf(void) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }

    size_t elf_size = (size_t)(_binary_user_program_elf_end - _binary_user_program_elf_start);

    shell_out("Writing embedded ELF (");
    shell_print_uint((uint64_t)elf_size);
    shell_out(" bytes) to program.elf...\n");

    if (!ext2_write_file("program.elf", _binary_user_program_elf_start, elf_size)) {
        shell_out("Write failed. (Does program.elf already exist on disk as a placeholder file?)\n");
        return;
    }

    shell_out("Saved. Run 'runfile program.elf' to load and execute it from disk.\n");
}

static void shell_cmd_runfile(const char *filename) {
    shell_out("\nMounting ext2 filesystem...\n");
    if (!shell_ext2_ensure_mounted()) {
        return;
    }

    size_t out_len = 0;
    if (!ext2_read_file(filename, g_elf_filebuf, sizeof(g_elf_filebuf), &out_len)) {
        shell_out("File not found or read error.\n");
        return;
    }

    shell_out("Loaded ");
    shell_print_uint((uint64_t)out_len);
    shell_out(" bytes from ");
    shell_out(filename);
    shell_out(", executing...\n");

    shell_launch_elf(g_elf_filebuf);
}

static void shell_cmd_mouse(void) {
    extern void mouse_get_state(int32_t *x, int32_t *y, uint8_t *buttons);
    int32_t x, y;
    uint8_t buttons;
    mouse_get_state(&x, &y, &buttons);
    shell_out("\nMouse: x=");
    shell_print_uint((uint64_t)x);
    shell_out(" y=");
    shell_print_uint((uint64_t)y);
    shell_out(" buttons=");
    shell_out((buttons & 0x01) ? "L" : "-");
    shell_out((buttons & 0x02) ? "R" : "-");
    shell_out((buttons & 0x04) ? "M" : "-");
    shell_out("\n");
}

static void shell_cmd_boxdemo(void) {
    extern void gui_box_demo(void);
    gui_box_demo();
}

static void shell_cmd_desktop(void) {
    extern void wm_demo_run(void);
    wm_demo_run();
}

static void shell_cmd_showmac(void) {
    uint8_t mac[6];
    eth_get_our_mac(mac);
    shell_out("\nCurrent MAC (randomized at boot): ");
    for (int i = 0; i < 6; i++) {
        shell_print_hex_byte(mac[i]);
        if (i < 5) shell_out_char(':');
    }
    shell_out("\n");
}

static void shell_cmd_macrandom(void) {
    eth_randomize_mac();
    shell_out("\nMAC re-randomized.\n");
    shell_cmd_showmac();
}

static void shell_execute(const char *cmd) {
    if (shell_strlen(cmd) == 0) {
        return;
    }

    if (shell_strcmp(cmd, "help") == 0) {
        shell_out("\nAvailable commands:\n");
        shell_out("  help     - show this list\n");
        shell_out("  print X  - print X back\n");
        shell_out("  meminfo  - show free physical memory\n");
        shell_out("  clear    - clear the screen\n");
        shell_out("  run      - load and execute the embedded user program\n");
        shell_out("  exit     - shut down the OS\n");
        shell_out("  lspci    - list PCI devices\n");
        shell_out("  ahci     - init AHCI controller and list ports\n");
        shell_out("  diskread - read LBA 0 from the SATA disk and hex-dump it\n");
        shell_out("  diskwrite - write+verify a test pattern to LBA 0\n");
        shell_out("  ls        - list files in ext2 root directory\n");
        shell_out("  cat X     - print contents of file X from ext2\n");
        shell_out("  vfscat X  - print file X via vfs_open/vfs_read (tests VFS->ext2 wiring)\n");
        shell_out("  ext2write X - overwrite file X in place (existing blocks only) and verify\n");
        shell_out("  tree      - recursively print the ext2 directory tree\n");
        shell_out("  ext2grow X - write >1 block of data to X, forcing new block allocation\n");
        shell_out("  saveelf   - write the embedded ELF program to program.elf on ext2\n");
        shell_out("  runfile X - load and execute an ELF binary from ext2 file X\n");
        shell_out("  showmac   - show current (randomized) MAC address\n");
        shell_out("  macrandom - generate and switch to a new random MAC\n");
        shell_out("  httpd     - start a minimal TCP/HTTP server on port 80\n");
        shell_out("  boxdemo   - drag a box around with the mouse (Esc to exit)\n");
        shell_out("  desktop   - multi-window desktop demo (Esc to exit)\n");
        shell_out("  mouse     - show current cursor position and button state\n");
    } else if (shell_strncmp(cmd, "print ", 6) == 0) {
        shell_out("\n");
        shell_out(cmd + 6);
        shell_out("\n");
    } else if (shell_strcmp(cmd, "print") == 0) {
        shell_out("\n");
    } else if (shell_strcmp(cmd, "meminfo") == 0) {
        uint64_t free_bytes = pmm_get_free_memory();
        shell_out("\nFree physical memory: ");
        shell_print_uint(free_bytes / 1024);
        shell_out(" KB\n");
    } else if (shell_strcmp(cmd, "clear") == 0) {
        if (fb_enabled) {
            fb_text_init(0, 255, 0, 0, 0, 0);
        }
    } else if (shell_strcmp(cmd, "run") == 0) {
        shell_cmd_run();
    } else if (shell_strcmp(cmd, "exit") == 0) {
        shell_cmd_exit();
    } else if (shell_strcmp(cmd, "lspci") == 0) {
        shell_cmd_lspci();
    } else if (shell_strcmp(cmd, "ahci") == 0) {
        shell_cmd_ahci();
    } else if (shell_strcmp(cmd, "diskread") == 0) {
        shell_cmd_diskread();
    } else if (shell_strcmp(cmd, "diskwrite") == 0) {
        shell_cmd_diskwrite();
    } else if (shell_strcmp(cmd, "ls") == 0) {
        shell_cmd_ls();
    } else if (shell_strncmp(cmd, "cat ", 4) == 0) {
        shell_cmd_cat(cmd + 4);
    } else if (shell_strncmp(cmd, "vfscat ", 7) == 0) {
        shell_cmd_vfscat(cmd + 7);
    } else if (shell_strncmp(cmd, "ext2write ", 10) == 0) {
        shell_cmd_ext2write(cmd + 10);
    } else if (shell_strcmp(cmd, "tree") == 0) {
        shell_cmd_tree();
    } else if (shell_strncmp(cmd, "ext2grow ", 9) == 0) {
        shell_cmd_ext2grow(cmd + 9);
    } else if (shell_strcmp(cmd, "saveelf") == 0) {
        shell_cmd_saveelf();
    } else if (shell_strncmp(cmd, "runfile ", 8) == 0) {
        shell_cmd_runfile(cmd + 8);
    } else if (shell_strcmp(cmd, "httpd") == 0) {
        shell_cmd_httpd();
    } else if (shell_strcmp(cmd, "boxdemo") == 0) {
        shell_cmd_boxdemo();
    } else if (shell_strcmp(cmd, "desktop") == 0) {
        shell_cmd_desktop();
    } else if (shell_strcmp(cmd, "showmac") == 0) {
        shell_cmd_showmac();
    } else if (shell_strcmp(cmd, "macrandom") == 0) {
        shell_cmd_macrandom();
    } else if (shell_strcmp(cmd, "mouse") == 0) {
        shell_cmd_mouse();
    } else {
        shell_out("\nUnknown command: ");
        shell_out(cmd);
        shell_out("\n");
    }
}

void shell_init(void) {
    shell_len = 0;
    shell_out("\nHASOS shell. Type 'help' for a list of commands.\n");
    shell_print_prompt();
}

void shell_handle_char(char c) {
    if (c == '\n') {
        shell_buf[shell_len] = '\0';
        shell_execute(shell_buf);
        shell_len = 0;
        shell_print_prompt();
        return;
    }

    if (c == '\b') {
        if (shell_len > 0) {
            shell_len--;
            shell_out_char('\b');
        }
        return;
    }

    if (shell_len < SHELL_BUF_SIZE - 1) {
        shell_buf[shell_len++] = c;
        shell_out_char(c);
    }
}

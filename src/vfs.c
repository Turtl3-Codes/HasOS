#include "vfs.h"
#include "ahci.h"
#include "ext2.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *s);

struct vfs_file {
    char name[VFS_MAX_NAME];
    uint8_t data[VFS_MAX_FILESIZE];
    size_t size;
    int used;
    int ext2_backed; // 1 if this file has a real dirent on ext2 (found via
                      // load_from_ext2), so vfs_close should flush writes
                      // back to disk. 0 for fresh in-memory-only files
                      // (ext2_write_file can't create new files, only
                      // write into existing ones).
};

struct vfs_fd {
    int file_index; // -1 if this fd slot is unused
    size_t position;
};

static struct vfs_file files[VFS_MAX_FILES];
static struct vfs_fd open_fds[VFS_MAX_OPEN];
static int ext2_ready = 0; // 0 = not tried, 1 = mounted, -1 = tried and failed

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

void vfs_init(void) {
    for (int i = 0; i < VFS_MAX_FILES; i++) files[i].used = 0;
    for (int i = 0; i < VFS_MAX_OPEN; i++) open_fds[i].file_index = -1;
    ext2_ready = 0;
    serial_print("VFS initialized\n");
}

static int find_file(const char *name) {
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (files[i].used && str_eq(files[i].name, name)) return i;
    }
    return -1;
}

static int create_file(const char *name) {
    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (!files[i].used) {
            str_copy(files[i].name, name, VFS_MAX_NAME);
            files[i].size = 0;
            files[i].used = 1;
            files[i].ext2_backed = 0;
            return i;
        }
    }
    return -1;
}

static int g_vfs_sata_port = -1;

static void vfs_find_sata_cb(const ahci_port_info_t *info) {
    if (g_vfs_sata_port == -1 && info->type == AHCI_DEV_SATA) {
        g_vfs_sata_port = info->port_number;
    }
}

static void vfs_try_mount_ext2(void) {
    if (ext2_ready != 0) {
        return; // already tried (success or failure), don't retry every open
    }

    if (!ahci_init()) {
        serial_print("VFS: no AHCI controller, ext2 unavailable\n");
        ext2_ready = -1;
        return;
    }

    g_vfs_sata_port = -1;
    ahci_scan_ports(vfs_find_sata_cb);
    if (g_vfs_sata_port == -1) {
        serial_print("VFS: no SATA drive, ext2 unavailable\n");
        ext2_ready = -1;
        return;
    }

    if (!ext2_mount(g_vfs_sata_port)) {
        serial_print("VFS: ext2 mount failed\n");
        ext2_ready = -1;
        return;
    }

    ext2_ready = 1;
    serial_print("VFS: ext2 mounted successfully\n");
}

// Attempts to load 'name' from the ext2 disk into a fresh in-memory file
// slot. Returns the file index on success, -1 on failure (not found,
// no ext2, out of slots, or file exceeds VFS_MAX_FILESIZE and gets
// truncated -- truncation still counts as success here, matching
// ext2_read_file's own truncate-on-overflow behavior). On success the
// slot is marked ext2_backed so vfs_close will flush writes back to disk.
static int load_from_ext2(const char *name) {
    vfs_try_mount_ext2();
    if (ext2_ready != 1) {
        return -1;
    }

    int idx = create_file(name);
    if (idx == -1) {
        return -1;
    }

    size_t out_len = 0;
    if (!ext2_read_file(name, files[idx].data, VFS_MAX_FILESIZE, &out_len)) {
        files[idx].used = 0; // roll back the slot, file doesn't exist on disk
        return -1;
    }

    files[idx].size = (out_len > VFS_MAX_FILESIZE) ? VFS_MAX_FILESIZE : out_len;
    files[idx].ext2_backed = 1;
    return idx;
}

int vfs_open(const char *name) {
    int file_idx = find_file(name);

    if (file_idx == -1) {
        file_idx = load_from_ext2(name);
    }

    if (file_idx == -1) {
        file_idx = create_file(name); // fall back to a fresh in-memory-only file
        if (file_idx == -1) return -1;
    }

    for (int i = 0; i < VFS_MAX_OPEN; i++) {
        if (open_fds[i].file_index == -1) {
            open_fds[i].file_index = file_idx;
            open_fds[i].position = 0;
            return i;
        }
    }
    return -1;
}

void vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN) return;

    int file_idx = open_fds[fd].file_index;
    if (file_idx == -1) {
        return;
    }

    struct vfs_file *f = &files[file_idx];
    if (f->ext2_backed) {
        if (ext2_write_file(f->name, f->data, f->size)) {
            serial_print("VFS: flushed ");
            serial_print(f->name);
            serial_print(" to ext2 on close\n");
        } else {
            serial_print("VFS: failed to flush ");
            serial_print(f->name);
            serial_print(" to ext2 on close\n");
        }
    }

    open_fds[fd].file_index = -1;
}

int vfs_read(int fd, void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || open_fds[fd].file_index == -1) return -1;
    struct vfs_file *f = &files[open_fds[fd].file_index];
    size_t remaining = f->size - open_fds[fd].position;
    size_t to_read = (len < remaining) ? len : remaining;
    for (size_t i = 0; i < to_read; i++) {
        ((uint8_t *)buf)[i] = f->data[open_fds[fd].position + i];
    }
    open_fds[fd].position += to_read;
    return (int)to_read;
}

int vfs_write(int fd, const void *buf, size_t len) {
    if (fd < 0 || fd >= VFS_MAX_OPEN || open_fds[fd].file_index == -1) return -1;
    struct vfs_file *f = &files[open_fds[fd].file_index];
    size_t space = VFS_MAX_FILESIZE - open_fds[fd].position;
    size_t to_write = (len < space) ? len : space;
    for (size_t i = 0; i < to_write; i++) {
        f->data[open_fds[fd].position + i] = ((const uint8_t *)buf)[i];
    }
    open_fds[fd].position += to_write;
    if (open_fds[fd].position > f->size) f->size = open_fds[fd].position;
    return (int)to_write;
}

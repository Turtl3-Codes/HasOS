#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include <stddef.h>

// ext2 directory entry file_type values (matches on-disk format).
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2

// Mounts an ext2 filesystem from the given AHCI port (reads superblock +
// block group descriptor table). Returns 1 on success.
int ext2_mount(int port_number);

// Lists entries in the root directory. Calls back with each name
// (null-terminated, name_len bytes copied) and its inode number.
typedef void (*ext2_dirent_cb)(const char *name, uint32_t inode_num, uint8_t file_type);
int ext2_list_root(ext2_dirent_cb callback);

// Lists entries in the directory at the given inode number. Same
// callback semantics as ext2_list_root (which is now just this called
// with the root inode). Returns entry count, or -1 on failure (bad
// inode, not mounted, etc). Caller is responsible for checking
// file_type == EXT2_FT_DIR before treating an entry as a directory to
// recurse into, and for skipping "." / ".." to avoid infinite loops.
int ext2_list_dir(uint32_t inode_num, ext2_dirent_cb callback);

// Reads a file by name from the root directory into dest (up to max_len
// bytes). Only direct blocks (first 12 block pointers) are supported --
// files larger than 12 * block_size will be truncated. Writes the
// actual file size to *out_len (may exceed max_len if truncated).
// Returns 1 on success (file found and read), 0 on failure.
int ext2_read_file(const char *name, uint8_t *dest, size_t max_len, size_t *out_len);

// Creates or overwrites a root-directory file, allocating direct blocks
// as needed up to the 12-direct-block limit. Indirect blocks are not
// supported. Updates the inode's size on disk on success.
// Returns 1 on success, 0 on failure (allocation, size, or disk error).
int ext2_write_file(const char *name, const uint8_t *data, size_t len);

// Finds and mounts the first SATA-backed ext2 disk.
int ext2_ensure_mounted(void);

#endif

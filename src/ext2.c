#include "ext2.h"
#include "ahci.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_print(const char *str);
extern int ahci_read_sectors(int port_number, uint64_t lba, uint16_t count, uint8_t *dest);
extern int ahci_write_sectors(int port_number, uint64_t lba, uint16_t count, const uint8_t *src);

#define EXT2_MAGIC       0xEF53
#define EXT2_ROOT_INODE  2
#define EXT2_MAX_BLOCK_SIZE 4096 // limited by ahci_read_sectors' 8-sector (4KB) cap

#define EXT2_S_IFREG 0x8000
#define EXT2_DEFAULT_FILE_MODE (EXT2_S_IFREG | 0644)

typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint8_t  reserved[1024 - 92];
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t osd1;
    uint32_t i_block[15];
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    // followed by name[name_len], not null-terminated
} __attribute__((packed)) ext2_dirent_t;

#define EXT2_DIRENT_HDR_SIZE 8 // sizeof(ext2_dirent_t), fixed part only

static int g_mounted = 0;
static int g_port = -1;
static ext2_superblock_t g_sb;
static uint32_t g_block_size;
static uint32_t g_inode_size;
static uint32_t g_bgdt_block;
static uint32_t g_num_groups;

static uint8_t g_blockbuf[EXT2_MAX_BLOCK_SIZE];
static uint8_t g_bitmapbuf[EXT2_MAX_BLOCK_SIZE];

static int ext2_read_block(uint32_t block_num, uint8_t *dest) {
    uint32_t sectors_per_block = g_block_size / 512;
    uint64_t lba = (uint64_t)block_num * sectors_per_block;
    return ahci_read_sectors(g_port, lba, (uint16_t)sectors_per_block, dest);
}

static int ext2_write_block(uint32_t block_num, const uint8_t *src) {
    uint32_t sectors_per_block = g_block_size / 512;
    uint64_t lba = (uint64_t)block_num * sectors_per_block;
    return ahci_write_sectors(g_port, lba, (uint16_t)sectors_per_block, src);
}

// Locates the block and in-block byte offset of group 'group's BGD
// entry, and reads that block into g_blockbuf. Returns a pointer into
// g_blockbuf on success, NULL on failure. Caller must not call any
// other g_blockbuf-using function before finishing with the pointer.
static ext2_bgd_t *ext2_load_bgd(uint32_t group, uint32_t *out_block) {
    if (group >= g_num_groups) {
        return NULL;
    }

    uint32_t bgd_byte_offset = group * sizeof(ext2_bgd_t);
    uint32_t bgd_block_offset = bgd_byte_offset / g_block_size;
    uint32_t bgd_offset_in_block = bgd_byte_offset % g_block_size;
    uint32_t target_block = g_bgdt_block + bgd_block_offset;

    if (!ext2_read_block(target_block, g_blockbuf)) {
        return NULL;
    }

    if (out_block) {
        *out_block = target_block;
    }
    return (ext2_bgd_t *)(g_blockbuf + bgd_offset_in_block);
}

static int ext2_read_inode(uint32_t inode_num, ext2_inode_t *out) {
    if (inode_num == 0) {
        return 0;
    }

    uint32_t index = inode_num - 1;
    uint32_t group = index / g_sb.s_inodes_per_group;
    uint32_t index_in_group = index % g_sb.s_inodes_per_group;

    uint32_t bgd_block;
    ext2_bgd_t *bgd = ext2_load_bgd(group, &bgd_block);
    if (!bgd) {
        return 0;
    }
    uint32_t inode_table_block = bgd->bg_inode_table;

    uint32_t byte_offset = index_in_group * g_inode_size;
    uint32_t block_offset = byte_offset / g_block_size;
    uint32_t offset_in_block = byte_offset % g_block_size;

    if (!ext2_read_block(inode_table_block + block_offset, g_blockbuf)) {
        return 0;
    }

    ext2_inode_t *inode = (ext2_inode_t *)(g_blockbuf + offset_in_block);
    *out = *inode;
    return 1;
}

// Writes an inode struct back to its position in the inode table.
// Does a read-modify-write of the containing block (since multiple
// inodes share each block).
static int ext2_write_inode(uint32_t inode_num, const ext2_inode_t *in) {
    if (inode_num == 0) {
        return 0;
    }

    uint32_t index = inode_num - 1;
    uint32_t group = index / g_sb.s_inodes_per_group;
    uint32_t index_in_group = index % g_sb.s_inodes_per_group;

    uint32_t bgd_block;
    ext2_bgd_t *bgd = ext2_load_bgd(group, &bgd_block);
    if (!bgd) {
        return 0;
    }
    uint32_t inode_table_block = bgd->bg_inode_table;

    uint32_t byte_offset = index_in_group * g_inode_size;
    uint32_t block_offset = byte_offset / g_block_size;
    uint32_t offset_in_block = byte_offset % g_block_size;

    uint32_t target_block = inode_table_block + block_offset;

    if (!ext2_read_block(target_block, g_blockbuf)) {
        return 0;
    }

    ext2_inode_t *inode = (ext2_inode_t *)(g_blockbuf + offset_in_block);
    *inode = *in;

    return ext2_write_block(target_block, g_blockbuf);
}

// Writes the in-memory superblock (g_sb) back to disk. Used after
// updating free-count fields on allocation.
static int ext2_write_superblock(void) {
    static uint8_t sb_buf[1024];
    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    *sb = g_sb;
    return ahci_write_sectors(g_port, 2, 2, sb_buf);
}

// Finds and allocates one free block via the block group bitmaps,
// starting from group 0. Marks the bit used, writes the bitmap back,
// decrements free-block counts in both the BGD and superblock, and
// persists both. Returns the allocated block's absolute block number,
// or 0 on failure (disk full or a disk error).
static uint32_t ext2_alloc_block(void) {
    for (uint32_t group = 0; group < g_num_groups; group++) {
        uint32_t bgd_block;
        ext2_bgd_t *bgd = ext2_load_bgd(group, &bgd_block);
        if (!bgd) {
            continue;
        }

        if (bgd->bg_free_blocks_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgd->bg_block_bitmap;
        uint32_t saved_bgd_block = bgd_block;

        if (!ext2_read_block(bitmap_block, g_bitmapbuf)) {
            continue;
        }

        uint32_t blocks_in_group = g_sb.s_blocks_per_group;
        if (group == g_num_groups - 1) {
            uint32_t total_accounted = group * g_sb.s_blocks_per_group;
            uint32_t remaining = g_sb.s_blocks_count - g_sb.s_first_data_block - total_accounted;
            if (remaining < blocks_in_group) {
                blocks_in_group = remaining;
            }
        }

        for (uint32_t bit = 0; bit < blocks_in_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint8_t bit_idx = (uint8_t)(bit % 8);

            if (byte_idx >= EXT2_MAX_BLOCK_SIZE) {
                break;
            }

            if (!(g_bitmapbuf[byte_idx] & (1 << bit_idx))) {
                g_bitmapbuf[byte_idx] |= (uint8_t)(1 << bit_idx);
                if (!ext2_write_block(bitmap_block, g_bitmapbuf)) {
                    return 0;
                }

                uint32_t block_num = g_sb.s_first_data_block + group * g_sb.s_blocks_per_group + bit;

                ext2_bgd_t *bgd2 = ext2_load_bgd(group, NULL);
                if (!bgd2) {
                    return 0;
                }
                bgd2->bg_free_blocks_count--;
                if (!ext2_write_block(saved_bgd_block, g_blockbuf)) {
                    return 0;
                }

                g_sb.s_free_blocks_count--;
                ext2_write_superblock();

                return block_num;
            }
        }
    }

    serial_print("EXT2: no free blocks (disk full)\n");
    return 0;
}

// Finds and allocates one free inode via the inode bitmaps, starting
// from group 0. Mirrors ext2_alloc_block but for inode numbers
// (1-indexed, unlike block numbers). Marks the bit used, decrements
// free-inode counts in both the BGD and superblock, and persists both.
// Returns the allocated inode number, or 0 on failure.
static uint32_t ext2_alloc_inode(void) {
    for (uint32_t group = 0; group < g_num_groups; group++) {
        uint32_t bgd_block;
        ext2_bgd_t *bgd = ext2_load_bgd(group, &bgd_block);
        if (!bgd) {
            continue;
        }

        if (bgd->bg_free_inodes_count == 0) {
            continue;
        }

        uint32_t bitmap_block = bgd->bg_inode_bitmap;
        uint32_t saved_bgd_block = bgd_block;

        if (!ext2_read_block(bitmap_block, g_bitmapbuf)) {
            continue;
        }

        uint32_t inodes_in_group = g_sb.s_inodes_per_group;

        for (uint32_t bit = 0; bit < inodes_in_group; bit++) {
            uint32_t byte_idx = bit / 8;
            uint8_t bit_idx = (uint8_t)(bit % 8);

            if (byte_idx >= EXT2_MAX_BLOCK_SIZE) {
                break;
            }

            if (!(g_bitmapbuf[byte_idx] & (1 << bit_idx))) {
                g_bitmapbuf[byte_idx] |= (uint8_t)(1 << bit_idx);
                if (!ext2_write_block(bitmap_block, g_bitmapbuf)) {
                    return 0;
                }

                // Inode numbers are 1-indexed and group-relative bit
                // position maps directly (unlike blocks, no
                // s_first_data_block offset for inodes).
                uint32_t inode_num = group * g_sb.s_inodes_per_group + bit + 1;

                ext2_bgd_t *bgd2 = ext2_load_bgd(group, NULL);
                if (!bgd2) {
                    return 0;
                }
                bgd2->bg_free_inodes_count--;
                if (!ext2_write_block(saved_bgd_block, g_blockbuf)) {
                    return 0;
                }

                g_sb.s_free_inodes_count--;
                ext2_write_superblock();

                return inode_num;
            }
        }
    }

    serial_print("EXT2: no free inodes\n");
    return 0;
}

static uint32_t align4(uint32_t n) {
    return (n + 3) & ~3U;
}

// Inserts a new directory entry (name -> inode_num) into the directory
// at dir_inode_num. Tries each of the directory's existing allocated
// blocks first, splitting a dirent's trailing free space (the gap
// between its real content size and its rec_len) to make room. If no
// existing block has space, allocates and formats a new block for the
// directory (up to the 12-direct-block ceiling). Returns 1 on success,
// 0 on failure (name too long, directory full, or a disk error).
static int ext2_insert_dirent(uint32_t dir_inode_num, const char *name, uint32_t new_inode_num, uint8_t file_type) {
    uint32_t name_len = 0;
    while (name[name_len] != '\0') {
        name_len++;
    }
    if (name_len == 0 || name_len > 255) {
        return 0;
    }

    uint32_t needed = align4(EXT2_DIRENT_HDR_SIZE + name_len);

    ext2_inode_t dir_inode;
    if (!ext2_read_inode(dir_inode_num, &dir_inode)) {
        return 0;
    }

    int allocated_blocks = 0;
    for (int b = 0; b < 12; b++) {
        if (dir_inode.i_block[b] != 0) {
            allocated_blocks++;
        } else {
            break;
        }
    }

    // Pass 1: look for room in an existing block by splitting a
    // dirent's trailing free space.
    for (int b = 0; b < allocated_blocks; b++) {
        uint32_t block_num = dir_inode.i_block[b];
        if (!ext2_read_block(block_num, g_blockbuf)) {
            serial_print("EXT2: failed to read directory block during insert\n");
            return 0;
        }

        uint32_t offset = 0;
        while (offset < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_blockbuf + offset);
            if (de->rec_len == 0) {
                break;
            }

            uint32_t ideal = (de->inode != 0) ? align4(EXT2_DIRENT_HDR_SIZE + de->name_len) : 0;
            uint32_t free_space = de->rec_len - ideal;

            if (de->inode != 0 && free_space >= needed) {
                // Split: shrink this entry to its ideal size, carve the
                // new entry out of the trailing free space.
                uint16_t old_rec_len = de->rec_len;
                de->rec_len = (uint16_t)ideal;

                ext2_dirent_t *new_de = (ext2_dirent_t *)(g_blockbuf + offset + ideal);
                new_de->inode = new_inode_num;
                new_de->rec_len = (uint16_t)(old_rec_len - ideal);
                new_de->name_len = (uint8_t)name_len;
                new_de->file_type = file_type;
                char *name_dest = (char *)new_de + EXT2_DIRENT_HDR_SIZE;
                for (uint32_t i = 0; i < name_len; i++) {
                    name_dest[i] = name[i];
                }

                return ext2_write_block(block_num, g_blockbuf);
            }

            if (de->inode == 0 && de->rec_len >= needed) {
                // Reused deleted-entry slot: occupy the whole rec_len.
                de->inode = new_inode_num;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                char *name_dest = (char *)de + EXT2_DIRENT_HDR_SIZE;
                for (uint32_t i = 0; i < name_len; i++) {
                    name_dest[i] = name[i];
                }

                return ext2_write_block(block_num, g_blockbuf);
            }

            offset += de->rec_len;
        }
    }

    // Pass 2: no existing block had room. Allocate a new block for the
    // directory, if there's a free direct block slot left.
    if (allocated_blocks >= 12) {
        serial_print("EXT2: directory full (12 direct blocks exhausted)\n");
        return 0;
    }

    uint32_t new_block = ext2_alloc_block();
    if (new_block == 0) {
        return 0;
    }

    for (uint32_t i = 0; i < g_block_size; i++) {
        g_blockbuf[i] = 0;
    }

    ext2_dirent_t *de = (ext2_dirent_t *)g_blockbuf;
    de->inode = new_inode_num;
    de->rec_len = (uint16_t)g_block_size; // spans the whole new block
    de->name_len = (uint8_t)name_len;
    de->file_type = file_type;
    char *name_dest = (char *)de + EXT2_DIRENT_HDR_SIZE;
    for (uint32_t i = 0; i < name_len; i++) {
        name_dest[i] = name[i];
    }

    if (!ext2_write_block(new_block, g_blockbuf)) {
        return 0;
    }

    dir_inode.i_block[allocated_blocks] = new_block;
    dir_inode.i_size += g_block_size;
    dir_inode.i_blocks += g_block_size / 512;

    return ext2_write_inode(dir_inode_num, &dir_inode);
}

// Allocates a new inode, initializes it as an empty regular file, and
// links it into the root directory under 'name'. Returns the new
// inode number on success, 0 on failure. Does not check whether a
// file with this name already exists -- caller's responsibility.
static uint32_t ext2_create_file(const char *name) {
    uint32_t inode_num = ext2_alloc_inode();
    if (inode_num == 0) {
        return 0;
    }

    ext2_inode_t inode;
    for (size_t i = 0; i < sizeof(inode); i++) {
        ((uint8_t *)&inode)[i] = 0;
    }
    inode.i_mode = EXT2_DEFAULT_FILE_MODE;
    inode.i_links_count = 1;

    if (!ext2_write_inode(inode_num, &inode)) {
        serial_print("EXT2: create failed, could not write new inode\n");
        return 0;
    }

    if (!ext2_insert_dirent(EXT2_ROOT_INODE, name, inode_num, EXT2_FT_REG_FILE)) {
        serial_print("EXT2: create failed, could not insert directory entry\n");
        // Note: the allocated inode is now leaked (bit stays marked
        // used, nothing points to it) -- acceptable for now, no
        // deallocation path exists yet.
        return 0;
    }

    return inode_num;
}

int ext2_mount(int port_number) {
    g_mounted = 0;
    g_port = port_number;

    // Superblock is always at byte offset 1024, exactly 1024 bytes,
    // regardless of filesystem block size -- read it via a fixed 512-byte
    // sector read (LBA 2, count 2) rather than ext2_read_block, since we
    // don't know the block size yet.
    static uint8_t sb_buf[1024];
    if (!ahci_read_sectors(g_port, 2, 2, sb_buf)) {
        serial_print("EXT2: failed to read superblock\n");
        return 0;
    }

    ext2_superblock_t *sb = (ext2_superblock_t *)sb_buf;
    if (sb->s_magic != EXT2_MAGIC) {
        serial_print("EXT2: bad magic, not an ext2 filesystem\n");
        return 0;
    }

    g_sb = *sb;
    g_block_size = 1024UL << g_sb.s_log_block_size;

    if (g_block_size > EXT2_MAX_BLOCK_SIZE) {
        serial_print("EXT2: block size too large (unsupported, max 4096)\n");
        return 0;
    }

    g_inode_size = (g_sb.s_rev_level == 0) ? 128 : g_sb.s_inode_size;
    g_bgdt_block = (g_block_size == 1024) ? 2 : 1;
    g_num_groups = (g_sb.s_blocks_count + g_sb.s_blocks_per_group - 1) / g_sb.s_blocks_per_group;

    g_mounted = 1;
    return 1;
}

int ext2_list_dir(uint32_t inode_num, ext2_dirent_cb callback) {
    if (!g_mounted) {
        return -1;
    }

    ext2_inode_t dir_inode;
    if (!ext2_read_inode(inode_num, &dir_inode)) {
        serial_print("EXT2: failed to read directory inode\n");
        return -1;
    }

    int count = 0;

    for (int b = 0; b < 12; b++) {
        uint32_t block_num = dir_inode.i_block[b];
        if (block_num == 0) {
            continue;
        }

        if (!ext2_read_block(block_num, g_blockbuf)) {
            serial_print("EXT2: failed to read directory block\n");
            return -1;
        }

        uint32_t offset = 0;
        while (offset < g_block_size) {
            ext2_dirent_t *de = (ext2_dirent_t *)(g_blockbuf + offset);
            if (de->rec_len == 0) {
                break;
            }
            if (de->inode != 0 && de->name_len > 0) {
                static char name_buf[256];
                uint8_t n = de->name_len;
                for (uint8_t i = 0; i < n; i++) {
                    name_buf[i] = ((char *)de)[sizeof(ext2_dirent_t) + i];
                }
                name_buf[n] = '\0';
                callback(name_buf, de->inode, de->file_type);
                count++;
            }
            offset += de->rec_len;
        }
    }

    return count;
}

int ext2_list_root(ext2_dirent_cb callback) {
    return ext2_list_dir(EXT2_ROOT_INODE, callback);
}

static int ext2_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static uint32_t g_find_target_inode;
static const char *g_find_target_name;

static void ext2_find_cb(const char *name, uint32_t inode_num, uint8_t file_type) {
    (void)file_type;
    if (g_find_target_inode == 0 && ext2_streq(name, g_find_target_name)) {
        g_find_target_inode = inode_num;
    }
}

int ext2_read_file(const char *name, uint8_t *dest, size_t max_len, size_t *out_len) {
    if (!g_mounted) {
        return 0;
    }

    g_find_target_inode = 0;
    g_find_target_name = name;
    if (ext2_list_root(ext2_find_cb) < 0) {
        serial_print("EXT2: failed to search root directory\n");
        return 0;
    }

    if (g_find_target_inode == 0) {
        serial_print("EXT2: file not found\n");
        return 0;
    }

    ext2_inode_t inode;
    if (!ext2_read_inode(g_find_target_inode, &inode)) {
        serial_print("EXT2: failed to read file inode\n");
        return 0;
    }

    size_t file_size = inode.i_size;
    size_t bytes_to_read = file_size;
    if (bytes_to_read > max_len) {
        bytes_to_read = max_len;
    }
    size_t direct_capacity = (size_t)12 * g_block_size;
    if (bytes_to_read > direct_capacity) {
        bytes_to_read = direct_capacity;
    }
    size_t written = 0;
    for (int b = 0; b < 12 && written < bytes_to_read; b++) {
        uint32_t block_num = inode.i_block[b];
        if (block_num == 0) {
            serial_print("EXT2: file has fewer allocated blocks than its size\n");
            return 0;
        }
        if (!ext2_read_block(block_num, g_blockbuf)) {
            serial_print("EXT2: failed to read file data block\n");
            return 0;
        }

        size_t remaining = bytes_to_read - written;
        size_t chunk = g_block_size;
        if (chunk > remaining) {
            chunk = remaining;
        }

        for (size_t i = 0; i < chunk; i++) {
            dest[written + i] = g_blockbuf[i];
        }
        written += chunk;
    }

    if (written != bytes_to_read) {
        serial_print("EXT2: incomplete file read\n");
        return 0;
    }
    if (out_len) {
        *out_len = file_size;
    }
    return 1;
}

int ext2_write_file(const char *name, const uint8_t *data, size_t len) {
    if (!g_mounted) {
        return 0;
    }

    g_find_target_inode = 0;
    g_find_target_name = name;
    if (ext2_list_root(ext2_find_cb) < 0) {
        serial_print("EXT2: failed to search root directory\n");
        return 0;
    }

    if (g_find_target_inode == 0) {
        // File doesn't exist yet -- create it rather than failing.
        uint32_t new_inode = ext2_create_file(name);
        if (new_inode == 0) {
            serial_print("EXT2: write failed, could not create file\n");
            return 0;
        }
        g_find_target_inode = new_inode;
    }

    ext2_inode_t inode;
    if (!ext2_read_inode(g_find_target_inode, &inode)) {
        serial_print("EXT2: write failed, could not read inode\n");
        return 0;
    }

    // Count already-allocated direct blocks.
    int allocated_blocks = 0;
    for (int b = 0; b < 12; b++) {
        if (inode.i_block[b] != 0) {
            allocated_blocks++;
        } else {
            break; // direct blocks are allocated contiguously
        }
    }

    size_t capacity = (size_t)allocated_blocks * g_block_size;

    // Grow the file if needed, up to the 12-direct-block ceiling.
    if (len > capacity) {
        size_t needed_blocks = (len + g_block_size - 1) / g_block_size;
        if (needed_blocks > 12) {
            serial_print("EXT2: write failed, file would exceed 12 direct blocks (no indirect block support yet)\n");
            return 0;
        }

        while ((size_t)allocated_blocks < needed_blocks) {
            uint32_t new_block = ext2_alloc_block();
            if (new_block == 0) {
                serial_print("EXT2: write failed, could not allocate new block\n");
                return 0;
            }
            inode.i_block[allocated_blocks] = new_block;
            inode.i_blocks += g_block_size / 512; // i_blocks counts 512-byte sectors
            allocated_blocks++;
        }

        capacity = (size_t)allocated_blocks * g_block_size;
    }

    size_t written = 0;
    for (int b = 0; b < allocated_blocks && written < len; b++) {
        uint32_t block_num = inode.i_block[b];

        size_t remaining = len - written;
        size_t chunk = (remaining < g_block_size) ? remaining : g_block_size;

        if (chunk < g_block_size) {
            // Partial final block: read-modify-write so we don't zero
            // out the untouched tail of the block.
            if (!ext2_read_block(block_num, g_blockbuf)) {
                serial_print("EXT2: write failed, could not read block for partial write\n");
                return 0;
            }
        }

        for (size_t i = 0; i < chunk; i++) {
            g_blockbuf[i] = data[written + i];
        }

        if (!ext2_write_block(block_num, g_blockbuf)) {
            serial_print("EXT2: write failed, disk write error\n");
            return 0;
        }

        written += chunk;
    }

    inode.i_size = (uint32_t)len;
    if (!ext2_write_inode(g_find_target_inode, &inode)) {
        serial_print("EXT2: write succeeded but inode update failed (size may be stale)\n");
        return 0;
    }

    return 1;
}

static int ensure_mounted_port = -1;

static void ensure_mounted_find_sata(const ahci_port_info_t *info) {
    if (ensure_mounted_port == -1 && info->type == AHCI_DEV_SATA) {
        ensure_mounted_port = info->port_number;
    }
}

int ext2_ensure_mounted(void) {
    if (!ahci_init()) {
        return 0;
    }
    ensure_mounted_port = -1;
    ahci_scan_ports(ensure_mounted_find_sata);
    if (ensure_mounted_port == -1) {
        return 0;
    }
    return ext2_mount(ensure_mounted_port);
}

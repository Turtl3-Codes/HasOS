#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_FILES     32
#define VFS_MAX_NAME      32
#define VFS_MAX_FILESIZE  4096
#define VFS_MAX_OPEN      16

void vfs_init(void);
int  vfs_open(const char *name);   // creates the file if it doesn't exist, returns fd
int  vfs_read(int fd, void *buf, size_t len);
int  vfs_write(int fd, const void *buf, size_t len);
void vfs_close(int fd);

#endif

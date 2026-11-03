#include "syscall.h"
#include "kernel_ctx.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_putchar(char c);
extern void serial_print(const char *str);
extern char keyboard_read_char(void);

extern int  vfs_open(const char *name);
extern int  vfs_read(int fd, void *buf, size_t len);
extern int  vfs_write(int fd, const void *buf, size_t len);
extern void vfs_close(int fd);
extern void process_wipe_current(void);

#define SYS_READ  0
#define SYS_WRITE 1
#define SYS_EXIT  2
#define SYS_OPEN  3
#define SYS_CLOSE 4

// User-facing fds 0/1/2 are reserved (stdin/stdout/stderr); real VFS
// files start at fd 3, offset from vfs_open's own 0-based fd table.
#define USER_FD_OFFSET 3

uint64_t syscall_dispatch(uint64_t sys_num, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    switch (sys_num) {
        case SYS_READ: {
            int fd = (int)arg1;
            uint8_t *buf = (uint8_t *)arg2;
            size_t len = (size_t)arg3;

            if (fd == 0) {
                // stdin: read up to len chars from the keyboard, stopping
                // at newline (included in the buffer) or when len is hit.
                size_t i = 0;
                while (i < len) {
                    char c = keyboard_read_char();
                    buf[i++] = (uint8_t)c;
                    if (c == '\n') {
                        break;
                    }
                }
                return (uint64_t)i;
            }

            if (fd < USER_FD_OFFSET) {
                return (uint64_t)-1; // stdout/stderr aren't readable
            }

            int n = vfs_read(fd - USER_FD_OFFSET, buf, len);
            return (uint64_t)n;
        }

        case SYS_WRITE: {
            int fd = (int)arg1;
            const uint8_t *buf = (const uint8_t *)arg2;
            size_t len = (size_t)arg3;

            if (fd == 1 || fd == 2) {
                for (size_t i = 0; i < len; i++) {
                    serial_putchar((char)buf[i]);
                }
                return (uint64_t)len;
            }

            if (fd < USER_FD_OFFSET) {
                return (uint64_t)-1; // stdin isn't writable
            }

            int n = vfs_write(fd - USER_FD_OFFSET, buf, len);
            return (uint64_t)n;
        }

        case SYS_EXIT: {
            serial_print("\n[user process exited via SYS_EXIT]\n");
            process_wipe_current();
            kernel_ctx_restore();   // does not return
            __builtin_unreachable();
        }

        case SYS_OPEN: {
            const char *name = (const char *)arg1;
            int vfd = vfs_open(name);
            if (vfd == -1) {
                return (uint64_t)-1;
            }
            return (uint64_t)(vfd + USER_FD_OFFSET);
        }

        case SYS_CLOSE: {
            int fd = (int)arg1;
            if (fd < USER_FD_OFFSET) {
                return 0; // no-op on stdin/stdout/stderr
            }
            vfs_close(fd - USER_FD_OFFSET);
            return 0;
        }

        default:
            return (uint64_t)-1;
    }
}

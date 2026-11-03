static inline long syscall3(long num, long arg1, long arg2, long arg3) {
    long ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r8", "r9", "r10", "r11", "memory"
    );
    return ret;
}

static long my_strlen(const char *s) {
    long n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

void _start(void) {
    const char *msg = "Hello from ring 3!\n";
    syscall3(1, 1, (long)msg, my_strlen(msg)); // write(1, msg, len)

    const char *fname = "greeting.txt";
    long fd = syscall3(3, (long)fname, 0, 0); // open(fname)

    if (fd >= 0) {
        const char *content = "Written from userland via syscalls!\n";
        syscall3(1, fd, (long)content, my_strlen(content)); // write(fd, content, len)

        // No lseek yet -- close and reopen to get a fresh fd at position 0.
        syscall3(4, fd, 0, 0); // close(fd)
        fd = syscall3(3, (long)fname, 0, 0); // open(fname) again

        static char buf[64];
        long n = syscall3(0, fd, (long)buf, sizeof(buf) - 1); // read(fd, buf, len)
        if (n > 0) {
            buf[n] = '\0';
            syscall3(1, 1, (long)"Read back: ", 11);
            syscall3(1, 1, (long)buf, n);
        }
        syscall3(4, fd, 0, 0); // close(fd)
    }

    syscall3(2, 0, 0, 0); // exit(0)
    while (1) {
        __asm__ volatile ("nop");
    }
}

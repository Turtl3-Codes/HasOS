# hasos

Hobby x86-64 OS aimed at privacy/anonymity, built from scratch.

Features: Limine boot, PMM/VMM, GDT/IDT, framebuffer GUI, PS/2 input, AHCI + ext2, VFS, ELF loading with ring-3 usermode, virtio-net with Ethernet/ARP/IP/ICMP/TCP, HTTP server.

## Build
Needs a cross-compiler/gcc, nasm, and Limine (v8.x binary branch):

    git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
    ./build.sh

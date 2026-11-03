#!/bin/bash
set -e

CC="gcc"
LD="ld"
CFLAGS="-ffreestanding -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -Isrc -std=c11"
LDFLAGS="-T linker.ld -nostdlib -no-pie -z max-page-size=0x1000"

USER_CFLAGS="-ffreestanding -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -Isrc -std=c11"
USER_LDFLAGS="-T src/user/user_linker.ld -nostdlib -no-pie -z max-page-size=0x1000"

SRC_DIR="src"
BUILD_DIR="build"
ISO_DIR="iso_root"

echo "=== Cleaning Old Artifacts ==="
rm -rf "$BUILD_DIR" hasos.elf hasos.iso

mkdir -p "$BUILD_DIR"
mkdir -p "$ISO_DIR/boot/limine"
mkdir -p "$ISO_DIR/limine"

echo "=== Compiling Kernel Sources ==="
for src in $(find "$SRC_DIR" -name "*.c" -not -path "*/user/*"); do
    obj="$BUILD_DIR/$(basename "${src%.c}.o")"
    echo "  CC  $src -> $obj"
    $CC $CFLAGS -c "$src" -o "$obj"
done

echo "=== Compiling Assembly Sources ==="
for src in $(find "$SRC_DIR" -name "*.asm" -o -name "*.s"); do
    obj="$BUILD_DIR/$(basename "${src%.*}.o")"
    echo "  AS  $src -> $obj"
    if [[ "$src" == *.asm ]]; then
        nasm -f elf64 "$src" -o "$obj"
    else
        $CC $CFLAGS -c "$src" -o "$obj"
    fi
done

echo "=== Building Embedded User Program ==="
mkdir -p "$BUILD_DIR/user"
$CC $USER_CFLAGS -c "$SRC_DIR/user/user_program.c" -o "$BUILD_DIR/user/user_program.o"
$LD $USER_LDFLAGS "$BUILD_DIR/user/user_program.o" -o "$BUILD_DIR/user/user_program.elf"

( cd "$BUILD_DIR/user" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 \
    user_program.elf user_program_blob.o )

echo "=== Linking Kernel ==="
$LD $LDFLAGS "$BUILD_DIR"/*.o "$BUILD_DIR/user/user_program_blob.o" -o hasos.elf

echo "=== Staging Files for ISO ==="
cp hasos.elf "$ISO_DIR/boot/hasos.elf"

cp limine.conf "$ISO_DIR/"
cp limine.conf "$ISO_DIR/boot/"

cp limine/limine-bios.sys "$ISO_DIR/boot/" 2>/dev/null || cp limine/limine-bios.sys "$ISO_DIR/limine/" 2>/dev/null || true
cp limine/limine-bios-cd.bin "$ISO_DIR/boot/" 2>/dev/null || cp limine/limine-bios-cd.bin "$ISO_DIR/limine/" 2>/dev/null || true
cp limine/limine-uefi-cd.bin "$ISO_DIR/boot/" 2>/dev/null || cp limine/limine-uefi-cd.bin "$ISO_DIR/limine/" 2>/dev/null || true

echo "=== Generating ISO ==="
xorriso -as mkisofs -b boot/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$ISO_DIR" -o hasos.iso

if [ -f "./limine/limine" ]; then
    ./limine/limine bios-install hasos.iso
elif command -v limine &> /dev/null; then
    limine bios-install hasos.iso
fi

echo "=== HASOS Build Complete ==="

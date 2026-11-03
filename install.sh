#!/bin/bash
set -e

BOOT_IMG="hasos_boot.img"
BOOT_SIZE_MB=64
PART_OFFSET="1M"

FULL_INSTALL=0
if [ "$1" == "--full" ] || [ ! -f "$BOOT_IMG" ]; then
    FULL_INSTALL=1
fi

if [ ! -f "hasos.elf" ]; then
    echo "hasos.elf not found. Run ./build.sh first."
    exit 1
fi

if [ "$FULL_INSTALL" == "1" ]; then
    echo "=== Creating $BOOT_IMG (${BOOT_SIZE_MB}MB) ==="
    dd if=/dev/zero of="$BOOT_IMG" bs=1M count="$BOOT_SIZE_MB" status=none

    echo "=== Partitioning (GPT, single FAT32 boot partition) ==="
    parted --script "$BOOT_IMG" \
        mklabel gpt \
        mkpart primary fat32 1MiB 100% \
        set 1 esp on

    echo "=== Formatting FAT32 boot partition ==="
    mformat -i "$BOOT_IMG@@${PART_OFFSET}" -F ::

    echo "=== Creating directory structure ==="
    mmd -i "$BOOT_IMG@@${PART_OFFSET}" ::/boot
    mmd -i "$BOOT_IMG@@${PART_OFFSET}" ::/boot/limine
    mmd -i "$BOOT_IMG@@${PART_OFFSET}" ::/limine
else
    echo "=== $BOOT_IMG already exists, doing a fast update (use --full to recreate) ==="
fi

echo "=== Copying kernel + config onto boot partition ==="
mcopy -o -i "$BOOT_IMG@@${PART_OFFSET}" hasos.elf ::/boot/hasos.elf
mcopy -o -i "$BOOT_IMG@@${PART_OFFSET}" limine.conf ::/limine.conf
mcopy -o -i "$BOOT_IMG@@${PART_OFFSET}" limine.conf ::/boot/limine.conf
mcopy -o -i "$BOOT_IMG@@${PART_OFFSET}" limine/limine-bios.sys ::/boot/limine-bios.sys

if [ "$FULL_INSTALL" == "1" ]; then
    echo "=== Installing Limine BIOS boot code ==="
    ./limine/limine bios-install "$BOOT_IMG"
fi

echo "=== Install complete: $BOOT_IMG is ready to boot ==="

#!/bin/sh
set -e
if [ -z "${SPIKEOS_ROOT:-}" ]; then
    SPIKEOS_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
    cd "$SPIKEOS_ROOT"
fi
. scripts/build.sh

# Build host tools
cc -o tools/mkinitrd tools/mkinitrd.c

# Build userland programs
make -C userland

# Create initrd
tools/mkinitrd initrd.img userland/*.elf

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/myos.kernel isodir/boot/myos.kernel
cp initrd.img isodir/boot/initrd.img
cat > isodir/boot/grub/grub.cfg << EOF
insmod all_video
set timeout=3
set default=0

menuentry "myos" {
	multiboot /boot/myos.kernel
	module /boot/initrd.img
}
EOF

# Build hybrid BIOS+UEFI ISO if x86_64-efi modules are available
# (run setup-efi.sh once to enable), otherwise BIOS-only as before
i686-elf-grub-mkrescue -o myos.iso isodir

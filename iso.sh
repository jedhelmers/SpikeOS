#!/bin/sh
set -e
. ./build.sh

# Build host tools
cc -o tools/mkinitrd tools/mkinitrd.c

# Build userland programs
i686-elf-gcc -nostdlib -ffreestanding -static -Wl,-Ttext=0x08048000 -o userland/hello.elf userland/hello.c

# Create initrd
tools/mkinitrd initrd.img userland/hello.elf

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/myos.kernel isodir/boot/myos.kernel
cp initrd.img isodir/boot/initrd.img
cat > isodir/boot/grub/grub.cfg << EOF
menuentry "myos" {
	multiboot /boot/myos.kernel
	module /boot/initrd.img
}
EOF
i686-elf-grub-mkrescue -o myos.iso isodir

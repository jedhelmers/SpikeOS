#!/bin/sh
set -e

# Parse flags
for arg in "$@"; do
    case "$arg" in
        -v|--verbose)
            export KERNEL_CPPFLAGS="-DVERBOSE_BOOT"
            ;;
    esac
done

. ./iso.sh

# Create disk image if it doesn't exist (64 MiB = 131072 sectors)
if [ ! -f disk.img ]; then
    dd if=/dev/zero of=disk.img bs=512 count=131072 2>/dev/null
    echo "Created 64 MiB disk image"
fi

# -serial pty allows for PTY psuedo-terminal
# This is required when developing UART interrupts
# qemu-system-$(./target-triplet-to-arch.sh $HOST) -cdrom myos.iso -serial pty
qemu-system-$(./target-triplet-to-arch.sh $HOST) \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -cdrom myos.iso \
    -device virtio-gpu-pci \
    -serial file:.debug.log \
    -no-reboot -no-shutdown \
    -d int,cpu_reset # -D qemu_debug.log

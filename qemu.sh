#!/bin/sh
set -e
. ./iso.sh

# -serial pty allows for PTY psuedo-terminal
# This is required when developing UART interrupts
# qemu-system-$(./target-triplet-to-arch.sh $HOST) -cdrom myos.iso -serial pty
qemu-system-$(./target-triplet-to-arch.sh $HOST) -cdrom myos.iso -device virtio-gpu-pci -serial file:.debug.log -no-reboot -no-shutdown -d int,cpu_reset # -D qemu_debug.log
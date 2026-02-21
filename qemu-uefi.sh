#!/bin/sh
# qemu-uefi.sh -- Build and run SpikeOS in QEMU with UEFI firmware (OVMF/EDK2)
#
# Uses qemu-system-x86_64 because OVMF is a 64-bit UEFI firmware.
# GRUB (x86_64-efi) on the ISO handles the transition from 64-bit long mode
# to 32-bit protected mode before loading the Multiboot kernel.
#
# Requires: hybrid ISO built with EFI support (run setup-efi.sh first)
set -e
. ./iso.sh

# OVMF firmware bundled with QEMU (Homebrew)
OVMF_CODE="/opt/homebrew/share/qemu/edk2-x86_64-code.fd"
OVMF_VARS="/opt/homebrew/share/qemu/edk2-i386-vars.fd"

if [ ! -f "$OVMF_CODE" ]; then
    echo "Error: OVMF firmware not found at $OVMF_CODE"
    echo "It should be bundled with QEMU. Try: brew reinstall qemu"
    exit 1
fi

if [ ! -f "$OVMF_VARS" ]; then
    echo "Error: OVMF variable store not found at $OVMF_VARS"
    echo "It should be bundled with QEMU. Try: brew reinstall qemu"
    exit 1
fi

# Copy the UEFI variable store to a temp file so the original stays clean
# (UEFI firmware writes boot variables to this store)
VARS_COPY="$(mktemp)"
cp "$OVMF_VARS" "$VARS_COPY"
trap "rm -f '$VARS_COPY'" EXIT

qemu-system-x86_64 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS_COPY" \
    -cdrom myos.iso \
    -vga std \
    -serial file:.debug.log \
    -no-reboot \
    -no-shutdown \
    -d int,cpu_reset

#!/bin/sh
# setup-efi.sh -- One-time setup to enable UEFI boot support
#
# Creates a symlink so i686-elf-grub-mkrescue can find x86_64-efi modules,
# allowing it to automatically produce hybrid BIOS+UEFI ISOs.
#
# Prerequisites:
#   brew install x86_64-elf-grub mtools
set -e
if [ -z "${SPIKEOS_ROOT:-}" ]; then
    SPIKEOS_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
    cd "$SPIKEOS_ROOT"
fi

# Resolve i686-elf-grub's module directory
I686_PREFIX="$(brew --prefix i686-elf-grub)"
I686_GRUB_PKGLIB="$I686_PREFIX/lib/i686-elf/grub"

if [ ! -d "$I686_GRUB_PKGLIB/i386-pc" ]; then
    echo "Error: i686-elf-grub not found at $I686_GRUB_PKGLIB"
    echo "Run: brew install i686-elf-grub"
    exit 1
fi

# Resolve x86_64-elf-grub's EFI module directory
X64_PREFIX="$(brew --prefix x86_64-elf-grub)"
X64_EFI_DIR="$X64_PREFIX/lib/x86_64-elf/grub/x86_64-efi"

if [ ! -d "$X64_EFI_DIR" ]; then
    echo "Error: x86_64-elf-grub not found."
    echo "Run: brew install x86_64-elf-grub"
    exit 1
fi

SYMLINK_TARGET="$I686_GRUB_PKGLIB/x86_64-efi"

if [ -L "$SYMLINK_TARGET" ]; then
    echo "Symlink already exists: $SYMLINK_TARGET"
    echo "  -> $(readlink "$SYMLINK_TARGET")"
elif [ -d "$SYMLINK_TARGET" ]; then
    echo "Directory already exists: $SYMLINK_TARGET (not a symlink, leaving alone)"
else
    echo "Creating symlink:"
    echo "  $SYMLINK_TARGET -> $X64_EFI_DIR"
    ln -s "$X64_EFI_DIR" "$SYMLINK_TARGET"
    echo "Done."
fi

# Symlink the unicode.pf2 font if missing (i686-elf-grub doesn't ship it)
I686_GRUB_SHARE="$I686_PREFIX/i686-elf/share/grub"
FONT_FILE="$I686_GRUB_SHARE/unicode.pf2"
X64_FONT="$X64_PREFIX/x86_64-elf/share/grub/unicode.pf2"

if [ ! -f "$FONT_FILE" ] && [ -f "$X64_FONT" ]; then
    echo "Symlinking missing unicode.pf2 font:"
    echo "  $FONT_FILE -> $X64_FONT"
    ln -s "$X64_FONT" "$FONT_FILE"
elif [ -f "$FONT_FILE" ]; then
    echo "Font unicode.pf2 already present."
fi

echo ""
echo "Platforms available to i686-elf-grub-mkrescue:"
for dir in "$I686_GRUB_PKGLIB"/*/; do
    echo "  $(basename "$dir")"
done

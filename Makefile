# SpikeOS top-level Makefile
# Thin wrapper around the build scripts in scripts/

.PHONY: all run run-verbose run-uefi run-uefi-verbose iso clean headers setup-efi

all: iso

run:
	@scripts/qemu.sh

run-verbose:
	@scripts/qemu.sh -v

run-uefi:
	@scripts/qemu-uefi.sh

run-uefi-verbose:
	@scripts/qemu-uefi.sh -v

iso:
	@scripts/iso.sh

clean:
	@scripts/clean.sh

headers:
	@scripts/headers.sh

setup-efi:
	@scripts/setup-efi.sh

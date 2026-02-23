#!/bin/sh
set -e
if [ -z "${SPIKEOS_ROOT:-}" ]; then
    SPIKEOS_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
    cd "$SPIKEOS_ROOT"
fi
. scripts/config.sh

for PROJECT in $PROJECTS; do
  (cd $PROJECT && $MAKE clean)
done

rm -rf sysroot
rm -rf isodir
rm -rf myos.iso

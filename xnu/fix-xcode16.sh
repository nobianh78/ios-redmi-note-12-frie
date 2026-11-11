#!/bin/bash
# Make xnu-8796 (macOS 13.5) build with Xcode 16's clang.
# Usage: fix-xcode16.sh <xnu-source-dir>
#
# 1. uio_stackbuf_t is `char` with 8-byte alignment; newer clang rejects arrays of it
#    ("size of array element ... isn't a multiple of its alignment"). Later xnu replaced it with the
#    UIO_STACKBUF() macro, an aligned char array; do the same at every declaration.
set -euo pipefail
src="${1:?xnu source dir}"

files=$(grep -rl 'uio_stackbuf_t' "$src/bsd" "$src/osfmk" "$src/iokit" "$src/security" 2>/dev/null || true)
for f in $files; do
  perl -pi -e 's/\buio_stackbuf_t\s+(\w+)\s*\[([^\]]+)\]/char $1\[$2\] __attribute__((aligned(_Alignof(union iovecs))))/g' "$f"
done
left=$(grep -rn 'uio_stackbuf_t\s\+\w\+\s*\[' $files 2>/dev/null || true)
if [ -n "$left" ]; then echo "fix-xcode16: unconverted declarations:"; echo "$left"; exit 1; fi
echo "fix-xcode16: converted uio_stackbuf_t arrays in $(echo "$files" | grep -c . || true) files"

# 2. Plain arm64 (no PAC) only: osfmk/arm/pmap/pmap.h declares `bool reserved10;` in both the
#    !CONFIG_ROSETTA and the !HAS_APPLE_PAC branches of struct pmap. Apple always builds arm64e, so the
#    duplicate never fires there. Rename the PAC placeholder (padding only, never referenced).
pmap_h="$src/osfmk/arm/pmap/pmap.h"
perl -0pi -e 's/(#if HAS_APPLE_PAC\s*\n\s*bool disable_jop;\s*\n#else\s*\n\s*bool )reserved10;/${1}reserved10_nopac;/' "$pmap_h"
grep -q 'reserved10_nopac' "$pmap_h" || { echo "fix-xcode16: pmap.h reserved10 fix did not apply"; exit 1; }
echo "fix-xcode16: renamed duplicate pmap reserved10 (no-PAC build)"

#!/bin/bash
# Make xnu-8796 (macOS 13.5) build with Xcode 16's clang as plain arm64 (no PAC) for non-Apple cores,
# entirely from open source.
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

# 3. Build the ARM64 machine-dependent layer from source instead of Apple's closed library.
#    The macOS config leaves the 20 files gated on nos_arm_asm / nos_arm_pmap in osfmk/conf/files.arm64
#    (start.s, locore.s, pmap, copyio, ...) out of the build and force-loads a prebuilt, arm64e-only
#    lib<MACHINE>.os.<CONFIG>.a from the KDK (makedefs/MakeInc.def). Enable the open sources, and drop
#    PPL (config_pmap_ppl): it needs Apple's APRR/GXF hardware, which Cortex cores do not have.
master="$src/config/MASTER.arm64.MacOSX"
perl -pi -e 's/^(#\s*KERNEL_BASE\s*=\s*\[\s*arm64) config_pmap_ppl /$1 nos_arm_asm nos_arm_pmap /' "$master"
grep -q 'KERNEL_BASE.*nos_arm_asm nos_arm_pmap' "$master" || { echo "fix-xcode16: MASTER nos_arm fix did not apply"; exit 1; }
! grep -q 'KERNEL_BASE.*config_pmap_ppl' "$master" || { echo "fix-xcode16: config_pmap_ppl still set"; exit 1; }

makedef="$src/makedefs/MakeInc.def"
perl -pi -e 's/^(\s*LDFLAGS_KERNEL_ONLY \+= -rdynamic) -Wl,-force_load,\$\(KDKROOT\)\/System\/Library\/KernelSupport\/lib\S+\.a\s*$/$1\n/' "$makedef"
! grep -q 'force_load,$(KDKROOT)/System/Library/KernelSupport' "$makedef" || { echo "fix-xcode16: closed lib force_load still present"; exit 1; }
echo "fix-xcode16: building arm64 machine layer from source (nos_arm_asm nos_arm_pmap, no PPL, no KDK lib)"

# 4. Three nos_arm files are not published: amcc_rorgn_ppl.c (PPL only, PPL is off), amcc_rorgn_common.c
#    (Apple AMCC read-only region; MT6768 has no AMCC) and kpc.c (Apple PMU counters).
#    Stub the AMCC region with the same semantics xnu uses when KTRR/CTRR is off, and supply a kpc.c
#    that reports zero counters (disabling kpc instead breaks ast.c, which always includes kperf_kpc.h).
cat > "$src/osfmk/arm64/amcc_rorgn_common.c" <<'EOF'
/* Stub for non-Apple SoCs: no AMCC/CTRR read-only region (matches xnu's !KTRR && !CTRR behaviour). */
#include <mach/vm_types.h>
#include <stdbool.h>
vm_offset_t rorgn_begin = 0;
vm_offset_t rorgn_end = 0;
void rorgn_stash_range(void) {}
void rorgn_lockdown(void) {}
bool rorgn_contains(vm_offset_t addr, vm_size_t size, bool defval) { (void)addr; (void)size; return defval; }
EOF
cp "$(dirname "$0")/kpc_stub.c" "$src/osfmk/arm64/kpc.c"
echo "fix-xcode16: stubbed amcc_rorgn_common.c and arm64 kpc.c"

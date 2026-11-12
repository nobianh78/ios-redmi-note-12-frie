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

/* Unpublished pmap-cs / I/O-filter hooks (declared in vm/pmap.h): no PPL, no code-signing monitor. */
int pmap_cs_configuration(void) { return 0; }
bool pmap_has_iofilter_protected_write(void) { return false; }

/* PPL monitor entry (arm_vm_init.c calls it unconditionally): nothing to enable without PPL. */
void ml_enable_monitor(void) {}
EOF
cp "$(dirname "$0")/kpc_stub.c" "$src/osfmk/arm64/kpc.c"
echo "fix-xcode16: stubbed amcc_rorgn_common.c and arm64 kpc.c"

# 5. pmap.c unconditionally includes arm64/ppl/sart.h and uat.h (unpublished) and calls only
#    sart_bootstrap() / uat_bootstrap(): Apple's SART and GPU UAT IOMMUs. No such hardware on MT6768.
mkdir -p "$src/osfmk/arm64/ppl"
for hw in sart uat; do
  HW=$(echo "$hw" | tr a-z A-Z)   # not ${hw^^}: macOS /bin/bash is 3.2
  cat > "$src/osfmk/arm64/ppl/$hw.h" <<EOF
/* Stub: Apple $HW hardware is absent on non-Apple SoCs (fix-xcode16.sh). */
#pragma once
static inline void ${hw}_bootstrap(void) {}
EOF
done
echo "fix-xcode16: stubbed arm64/ppl/sart.h and uat.h"

# 6. pmap.c's !XNU_MONITOR (no PPL) branch has rotted, since Apple builds this config with PPL:
#    a. ptep_get_iommu() (unpublished) is only used in panic messages: print the PTE pointer instead.
#    b. pmap_phys_write_disable() is called before its static definition: add a forward declaration.
#    c. pmap_ppl_(un)lockdown_pages() are PPL-only page lockdown: without PPL there is nothing to do.
pmap_c="$src/osfmk/arm/pmap/pmap.c"
perl -pi -e 's/ptep_get_iommu\(pte_p\)/(void *)(pte_p)/g' "$pmap_c"
perl -0pi -e 's/(#include <sys\/cdefs\.h>\n)/$1\nstatic void pmap_phys_write_disable(vm_address_t va); \/* fix-xcode16 *\/\n/' "$pmap_c"
perl -0pi -e 's/#else\n(\s*)pmap_ppl_(un)?lockdown_pages\(kva, size, 0, ppl_writable\);\n/#else\n$1(void)kva; (void)size; (void)ppl_writable; \/* no PPL: no lockdown *\/\n/g' "$pmap_c"
! grep -q 'ptep_get_iommu' "$pmap_c" || { echo "fix-xcode16: ptep_get_iommu left"; exit 1; }
grep -q 'static void pmap_phys_write_disable(vm_address_t va); /\* fix-xcode16' "$pmap_c" || { echo "fix-xcode16: forward decl missing"; exit 1; }
test "$(grep -c 'no PPL: no lockdown' "$pmap_c")" -eq 2 || { echo "fix-xcode16: lockdown stubs not applied"; exit 1; }
echo "fix-xcode16: patched pmap.c no-PPL branch"

# 7. Assembly touches Apple-only system registers by name (CPU_OVRD, CPU_CFG, ...). VMAPPLE has no
#    header defining them, and Cortex cores do not implement them. Architectural registers are named
#    *_EL0..3 or are one of a few PSTATE/special names; anything else is Apple's: reads give 0,
#    writes are dropped (same rule as for implementation-defined registers elsewhere in this port).
for s in "$src"/osfmk/arm64/*.s "$src"/osfmk/arm/*.s; do
  [ -f "$s" ] || continue
  perl -pi -e '
    BEGIN { %arch = map { $_ => 1 } qw(DAIF DAIFSet DAIFClr NZCV FPCR FPSR SPSel CurrentEL PAN UAO DIT SSBS TCO) }
    sub apple { my $r = shift; $r =~ /^[A-Z][A-Z0-9_]*$/ && $r !~ /_EL[0-3]$/ && !$arch{$r} }
    if (/^(\s*)mrs\s+(x\d+|w\d+)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\/\/.*)?$/ && apple($3)) {
      $_ = "$1mov\t$2, #0\t// fix-xcode16: Apple-only $3\n"; print STDERR "  $ARGV:$.: mrs $3\n";
    } elsif (/^(\s*)msr\s+([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(x\d+|w\d+|#\S+)\s*(\/\/.*)?$/ && apple($2)) {
      $_ = "$1nop\t\t// fix-xcode16: Apple-only $2\n"; print STDERR "  $ARGV:$.: msr $2\n";
    }
    close ARGV if eof;' "$s"
done
echo "fix-xcode16: neutralized Apple-only system registers in arm/arm64 assembly"

# 8. The ARM64 boot core is gated on PPL: `optional a b` needs every option, and arm_init.c,
#    arm_vm_init.c and start.s all list config_pmap_ppl. Their PPL parts are #if XNU_MONITOR inside,
#    so build them on nos_arm_asm alone. Without them _start, gPhysBase, phystokv... are missing.
files_arm64="$src/osfmk/conf/files.arm64"
perl -pi -e 's/^(osfmk\/arm\/arm_init\.c\s+)optional config_pmap_ppl\s*$/$1optional nos_arm_asm\n/;
              s/^(osfmk\/arm64\/arm_vm_init\.c\s+)optional config_pmap_ppl\s*$/$1optional nos_arm_asm\n/;
              s/^(osfmk\/arm64\/start\.s\s+)optional nos_arm_asm config_pmap_ppl\s*$/$1optional nos_arm_asm\n/' "$files_arm64"
for f in arm/arm_init.c arm64/arm_vm_init.c arm64/start.s; do
  grep -qE "^osfmk/$f[[:space:]]+optional nos_arm_asm\$" "$files_arm64" || { echo "fix-xcode16: $f gate not changed"; exit 1; }
done

#    Apple IOMMU descriptors (DART/SART/UAT/NVMe-PPL) and pmap_iommu_* only live in the export list
#    for Apple's kexts; nothing in the kernel calls them, and their code is unpublished. Unexport them.
exports="$src/config/Private.arm64.exports"
perl -ni -e 'print unless /^_(pmap_iommu_\w+|nvme_ppl_get_desc|sart_get_desc|uat_get_desc|t\d+dart_\w+)\s*$/' "$exports"
! grep -qE '^_(pmap_iommu_|t[0-9]+dart_|sart_get_desc|uat_get_desc|nvme_ppl_get_desc)' "$exports" || { echo "fix-xcode16: IOMMU exports left"; exit 1; }
#    Same for the C++ IOUnifiedAddressTranslator (IOKit side of Apple's GPU UAT): exported, no source.
perl -ni -e 'print unless /IOUnifiedAddressTranslator/' "$exports"
! grep -q 'IOUnifiedAddressTranslator' "$exports" || { echo "fix-xcode16: IOUnifiedAddressTranslator exports left"; exit 1; }
echo "fix-xcode16: built boot core without PPL gate, unexported Apple IOMMU descriptors"

# 9. start.s includes arm64/tunables/tunables.s (unpublished) for one macro, APPLY_TUNABLES, which
#    writes per-Apple-SoC HID settings chosen by MIDR. Cortex cores need none: empty macro.
mkdir -p "$src/osfmk/arm64/tunables"
cat > "$src/osfmk/arm64/tunables/tunables.s" <<'EOF'
/* Stub (fix-xcode16.sh): no Apple per-SoC HID tunables on non-Apple cores. */
.macro APPLY_TUNABLES midr, tmp1, tmp2
.endmacro
EOF
echo "fix-xcode16: stubbed arm64/tunables/tunables.s"

# 10. With APPLE_ARM64_ARCH_FAMILY (set for VMAPPLE) CleanPoC_DcacheRegion_Force calls the unpublished
#     CleanPoC_DcacheRegion_Force_nopreempt[_nohid]. Cortex caches really must be cleaned, so route both
#     to the file-local cvac/civac loop CleanPoC_DcacheRegion_internal (not global, hence defined here).
caches="$src/osfmk/arm64/caches_asm.s"
cat >> "$caches" <<'EOF'

/* fix-xcode16: unpublished nopreempt variants -> real cache clean loop (callers already block preemption) */
	.text
	.align 2
	.globl EXT(CleanPoC_DcacheRegion_Force_nopreempt)
	.globl EXT(CleanPoC_DcacheRegion_Force_nopreempt_nohid)
LEXT(CleanPoC_DcacheRegion_Force_nopreempt)
LEXT(CleanPoC_DcacheRegion_Force_nopreempt_nohid)
	b		EXT(CleanPoC_DcacheRegion_internal)
EOF
grep -q 'globl EXT(CleanPoC_DcacheRegion_Force_nopreempt_nohid)' "$caches" || { echo "fix-xcode16: cache clean stubs missing"; exit 1; }
echo "fix-xcode16: added CleanPoC_DcacheRegion_Force_nopreempt[_nohid], ml_enable_monitor"

# 11. Boot breadcrumbs for the Redmi 12 ("fire") bring-up. Each DF_MARK paints a black band on the
#     visible scanout buffer: OVL layer 3 (physical 0x7ce6a000, pitch 4320), which sits on top of
#     layer 0 (0x7c430000) and hides it, as measured on the phone. So the furthest
#     stage reached is visible without a UART. MMU-off stages only (physical address). Uses x16/x17 and
#     cbnz (no flags), unique labels via \@. Drop this step once XNU has a real console.
start_s="$src/osfmk/arm64/start.s"
perl -0pi -e 's|(#include <arm64/exception_asm.h>\n)|$1
/* fix-xcode16: DF_MARK n - bright band at rows 1300+60n..+40 of fire OVL layer 3 (MMU off only).
 * Pixel value = low 32 bits of its own address (0x7ce6xxxx -> pink/red), visible on the dark blue
 * console background; black was too close to it to read in photos. */
.macro DF_MARK n
	ldr		x16, =(0x7ce6a000 + (1300 + (\\n * 60)) * 4320)
	mov		x17, #((40 * 4320) / 4)
df_mark_loop\\\@:
	str		w16, [x16]		/* no writeback form: base==source is UNPREDICTABLE */
	add		x16, x16, #4
	sub		x17, x17, #1
	cbnz	x17, df_mark_loop\\\@
	dsb		sy
.endmacro
|' "$start_s"
perl -pi -e '
  $in = 1 if /^LEXT\(start_first_cpu\)/;
  if ($in && !$m0 && /^\s*msr\s+DAIFSet, #\(DAIFSC_ALL\)/) { $_ .= "\tDF_MARK 0\n"; $m0 = 1; }
  if ($in && !$m1 && /^\s*ldr\s+x26, \[x20, BA_BOOT_FLAGS\]/) { $_ .= "\tDF_MARK 1\n"; $m1 = 1; }
  if ($in && !$m2 && /^\s*b\s+common_start\s*$/) { $_ = "\tDF_MARK 2\n$_"; $m2 = 1; }
  if (!$m3 && /^\s*MOV64\s+x0, SCTLR_EL1_DEFAULT/) { $_ = "\tDF_MARK 3\n$_"; $m3 = 1; }
' "$start_s"
test "$(grep -c '^	DF_MARK [0-3]$' "$start_s")" -eq 4 || { echo "fix-xcode16: expected 4 DF_MARKs in start.s"; grep -n DF_MARK "$start_s"; exit 1; }
grep -q '^\.macro DF_MARK n' "$start_s" || { echo "fix-xcode16: DF_MARK macro missing"; exit 1; }
echo "fix-xcode16: inserted 4 boot breadcrumbs (DF_MARK) in start.s"

# 12. Post-MMU breadcrumbs (fire showed all 4 MMU-off bands, then nothing).
#     a. After the V=P bootstrap mapping is built, x2 points at its (single, 16K granule: one L1 entry
#        covers 64 GB) L2 table. Add one 32 MB block V=P mapping 0x7c000000, which holds OVL layers
#        0 and 3. Calling create_bootstrap_mapping again would allocate a new L2 and replace the L1
#        entry, dropping the kernel's own mapping. Scratch: x16, x17, x6 (x6 is dead here).
#     b. DF_MARK_C: same band as DF_MARK, then clean it to PoC (caches are on after SCTLR.M/C).
perl -0pi -e 's|(\tcreate_bootstrap_mapping x0,  x4,  x5, x1, x2, x6, x10, x11, x12, x13\n)|$1\t/* fix-xcode16: V=P 32MB block for the fire framebuffer (post-MMU DF_MARK) */\n\tMOV64\tx16, 0x7c000000\n\tand\t\tx17, x16, #(ARM_TT_L2_INDEX_MASK)\n\tlsr\t\tx17, x17, #(ARM_TT_L2_SHIFT)\n\tlsl\t\tx17, x17, #(TTE_SHIFT)\n\tadd\t\tx17, x2, x17\n\tmov\t\tx6, #(ARM_TTE_BOOT_BLOCK)\n\torr\t\tx6, x6, x16\n\tstr\t\tx6, [x17]\n|' "$start_s"
grep -q 'V=P 32MB block for the fire framebuffer' "$start_s" || { echo "fix-xcode16: fb V=P block not inserted"; exit 1; }
perl -0pi -e 's|(\.macro DF_MARK n\n.*?\.endmacro\n)|$1
/* fix-xcode16: DF_MARK_C n - DF_MARK for MMU-on code (V=P fb block): clean the band to PoC */
.macro DF_MARK_C n
	DF_MARK \\n
	ldr		x16, =(0x7ce6a000 + (1300 + (\\n * 60)) * 4320)
	mov		x17, #((40 * 4320) / 64)
df_markc_loop\\\@:
	dc		civac, x16
	add		x16, x16, #64
	sub		x17, x17, #1
	cbnz	x17, df_markc_loop\\\@
	dsb		sy
.endmacro
|s' "$start_s"
perl -0pi -e 's|(\tMSR_SCTLR_EL1_X0\n\tisb\t\tsy\n)|$1\tDF_MARK_C 4\n|' "$start_s"
grep -q '^	DF_MARK_C 4$' "$start_s" || { echo "fix-xcode16: DF_MARK_C 4 not inserted"; grep -n -A2 'MSR_SCTLR_EL1_X0' "$start_s"; exit 1; }

#     c. C breadcrumbs in arm_init() (TTBR0 still holds the V=P tables until arm_vm_init()).
cat >> "$src/osfmk/arm64/amcc_rorgn_common.c" <<'EOF'

/* fire bring-up breadcrumb for C code running on the bootstrap V=P tables (see start.s DF_MARK). */
void df_mark(int n);
void
df_mark(int n)
{
	/* the V=P tables get rebuilt in arm_vm_init: drop any stale TLB entry for the framebuffer block */
	__asm__ volatile ("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(0x7ce6a000ULL + (1300ULL + (uint64_t)n * 60) * 4320);
	for (int i = 0; i < (40 * 4320) / 4; i++) {
		p[i] = (uint32_t)(uintptr_t)&p[i];
	}
	for (uintptr_t a = (uintptr_t)p; a < (uintptr_t)p + 40 * 4320; a += 64) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}
EOF
arm_init="$src/osfmk/arm/arm_init.c"
perl -0pi -e 's|^|extern void df_mark(int); /* fix-xcode16: fire bring-up breadcrumbs */\n|' "$arm_init"
perl -pi -e '
  if (!$a && /^\tPE_init_platform\(FALSE, args\);/) { $_ = "\tdf_mark(5);\n$_\tdf_mark(6);\n"; $a = 1; }
  elsif (!$b && /^\tml_parse_cpu_topology\(\);/) { $_ .= "\tdf_mark(7);\n"; $b = 1; }
  elsif (!$c && /^\tarm_vm_init\(xmaxmem, args\);/) { $_ = "\tdf_mark(8);\n$_"; $c = 1; }
' "$arm_init"
test "$(grep -c 'df_mark([5-8]);' "$arm_init")" -eq 4 || { echo "fix-xcode16: expected 4 df_mark calls in arm_init.c"; grep -n df_mark "$arm_init"; exit 1; }
echo "fix-xcode16: post-MMU breadcrumbs: V=P fb block, DF_MARK_C 4, df_mark 5-8 in arm_init"

#     d. On fire XNU got through arm_vm_init's entry (9 bands). arm_vm_init then drops the bootstrap
#        V=P tables (TTBR0 <- invalid_ttep), which removes the framebuffer mapping the breadcrumbs use.
#        Bring-up only: keep TTBR0 on the bootstrap tables (no user space exists yet) and add
#        breadcrumbs 9-12 after arm_vm_init, PE_init_printf, PE_init_platform(TRUE), before machine_startup.
arm_vm_init_c="$src/osfmk/arm64/arm_vm_init.c"
perl -0pi -e 's|\tset_mmu_ttb\(invalid_ttep & TTBR_BADDR_MASK\);\n|\t/* fix-xcode16: fire bring-up keeps the bootstrap V=P tables in TTBR0 for df_mark */\n|' "$arm_vm_init_c"
grep -q 'fire bring-up keeps the bootstrap V=P tables' "$arm_vm_init_c" || { echo "fix-xcode16: TTBR0 keep not applied"; exit 1; }
#        arm_replace_identity_map() bzero()s the bootstrap V=P L2 table (keeping one code page), which
#        also erases the framebuffer block, so every df_mark after it faulted. Put the block back into
#        the rebuilt L2 (fb 0x7c000000 shares the kernel's L1 entry, index 0).
perl -0pi -e 's#(\t\*tte2 = ARM_TTE_BOOT_TABLE \| \(l3_ptp_phys & ARM_TTE_TABLE_MASK\);\n)#$1\tl2_ptp_virt[L2_TABLE_INDEX(0x7c000000ULL)] = ARM_TTE_BOOT_BLOCK | 0x7c000000ULL; /* fix-xcode16: fire fb for df_mark */\n#' "$arm_vm_init_c"
grep -q '^	\*tte2 = ARM_TTE_BOOT_TABLE | (l3_ptp_phys & ARM_TTE_TABLE_MASK);$' "$arm_vm_init_c" || { echo "fix-xcode16: tte2 line damaged"; exit 1; }
grep -q '^	l2_ptp_virt\[L2_TABLE_INDEX(0x7c000000ULL)\] = ARM_TTE_BOOT_BLOCK | 0x7c000000ULL;' "$arm_vm_init_c" || { echo "fix-xcode16: fb block not restored in arm_replace_identity_map"; exit 1; }
#        Still 9 bands with the fb block restored: breadcrumbs 13-18 inside arm_vm_init itself.
perl -0pi -e 's|^|extern void df_mark(int); /* fix-xcode16: fire bring-up breadcrumbs */\n|' "$arm_vm_init_c"
perl -pi -e '
  if (!$h && /^\tavail_start = args->topOfKernelData;/) { $_ .= "\tdf_mark(13);\n"; $h = 1; }
  elsif (!$i && m{^\t/\* Initialize invalid tte page \*/}) { $_ = "\tdf_mark(14);\n$_"; $i = 1; }
  elsif (!$j && /^\tarm_vm_prot_init\(args\);/) { $_ = "\tdf_mark(15);\n$_"; $j = 1; }
  elsif (!$k && /^\tarm_vm_physmap_init\(args\);/) { $_ = "\tdf_mark(16);\n$_"; $k = 1; }
  elsif (!$l && /^\tset_mmu_ttb_alternate\(cpu_ttep & TTBR_BADDR_MASK\);/) { $_ .= "\tdf_mark(17);\n"; $l = 1; }
  elsif (!$m && /^\tkva_active = TRUE;/) { $_ .= "\tdf_mark(18);\n"; $m = 1; }
' "$arm_vm_init_c"
test "$(grep -c 'df_mark(1[3-8]);' "$arm_vm_init_c")" -eq 6 || { echo "fix-xcode16: expected df_mark 13-18 in arm_vm_init.c"; grep -n df_mark "$arm_vm_init_c"; exit 1; }
echo "fix-xcode16: df_mark 13-18 inside arm_vm_init"
#        16 bands on fire: all of arm_vm_init (13-18) and mark 9 after it, then nothing. Marks 10-12
#        sit inside the arm_vm_init -> PE_init_printf stretch (screen has room for 19 bands only).
perl -pi -e '
  if (!$d && /^\tarm_vm_init\(xmaxmem, args\);/) { $_ .= "\tdf_mark(9);\n"; $d = 1; }
  elsif (!$e && /^\tPE_consistent_debug_inherit\(\);/) { $_ .= "\tdf_mark(10);\n"; $e = 1; }
  elsif (!$f && /^\tconst unsigned int serial_exists = serial_init\(\);/) { $_ .= "\tdf_mark(11);\n"; $f = 1; }
  elsif (!$g && /^\tkernel_startup_initialize_upto\(STARTUP_SUB_KPRINTF\);/) { $_ .= "\tdf_mark(12);\n"; $g = 1; }
' "$arm_init"
test "$(grep -c 'df_mark(\(9\|1[0-2]\));' "$arm_init")" -eq 4 || { echo "fix-xcode16: expected df_mark 9-12 in arm_init.c"; grep -n df_mark "$arm_init"; exit 1; }
echo "fix-xcode16: kept V=P after arm_vm_init, df_mark 9-12"

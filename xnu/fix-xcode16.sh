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

#include "df_font8x8.h"
/* fire bring-up: print a panic string (2x 8x8 font, white on black) at the bottom of the L3 layer. */
void df_puts(const char *s, unsigned long caller);
void
df_puts(const char *s, unsigned long caller)
{
	__asm__ volatile ("dsb ishst; tlbi vmalle1; dsb ish; isb" ::: "memory");
	volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)0x7ce6a000ULL;
	/* 1x 8x8 font, 9px lines: 135 cols x 46 rows, room for the full arm64 register dump */
	const unsigned pitch = 4320 / 4, top = 2040, bottom = 2460, cols = 1080 / 8;
	char hex[26] = "caller 0x";
	for (int i = 0; i < 16; i++) {
		hex[9 + i] = "0123456789abcdef"[(caller >> (60 - 4 * i)) & 0xf];
	}
	hex[25] = 0;
	for (unsigned y = top; y < bottom; y++) {
		for (unsigned x = 0; x < 1080; x++) {
			fb[y * pitch + x] = 0xff000000;
		}
	}
	unsigned col = 0, row = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (const char *c = pass ? hex : s; *c && top + (row + 1) * 9 < bottom; c++) {
			if (*c == '\n' || col == cols) { col = 0; row++; if (*c == '\n') continue; }
			unsigned ch = (*c == '\t') ? ' ' : (*c < 0x20 || *c > 0x7e) ? '?' : (unsigned)*c;
			for (int fy = 0; fy < 8; fy++) {
				uint8_t bits = font8x8[ch - 0x20][fy];
				for (int fx = 0; fx < 8; fx++) {
					if (bits & (1 << fx)) {
						fb[(top + row * 9 + fy) * pitch + col * 8 + fx] = 0xffffffff;
					}
				}
			}
			col++;
		}
		col = 0; row++;
	}
	for (uintptr_t a = (uintptr_t)&fb[top * pitch]; a < (uintptr_t)&fb[bottom * pitch]; a += 64) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}
EOF
cp "$(dirname "$0")/font8x8.h" "$src/osfmk/arm64/df_font8x8.h"
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
#        (Breadcrumbs 13-18 inside arm_vm_init all showed on fire once /chosen random-seed was added;
#        removed, their screen rows are reused below.)
#        Fire progress so far: everything through kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF)
#        (19 bands). Next stretch: console creation, printf, interrupts. The screen fits 19 bands, so
#        marks 10-13 now follow PE_create_console, PE_init_printf, PE_init_platform(TRUE) and precede
#        machine_startup.
perl -pi -e '
  if (!$d && /^\tarm_vm_init\(xmaxmem, args\);/) { $_ .= "\tdf_mark(9);\n"; $d = 1; }
  elsif (!$e && /^\tPE_create_console\(\);/) { $_ .= "\tdf_mark(10);\n"; $e = 1; }
' "$arm_init"
test "$(grep -c 'df_mark(\(9\|10\));' "$arm_init")" -eq 2 || { echo "fix-xcode16: expected df_mark 9-10 in arm_init.c"; grep -n df_mark "$arm_init"; exit 1; }
#        Fire stopped inside PE_create_console (10 bands). Marks 11-13 go inside initialize_screen:
#        after the framebuffer io_map, after gc_initialize, after the op switch.
vc_c="$(dirname "$arm_init")/../console/video_console.c"
perl -0pi -e 's|^|extern void df_mark(int); /* fix-xcode16: fire bring-up breadcrumbs */\n|' "$vc_c"
perl -pi -e '
  if (!$a && /^\s+new_vinfo\.v_physaddr, new_vinfo\.v_width.*BRINGUP/) { $_ .= "\t\t\tdf_mark(11);\n"; $a = 1; }
  elsif (!$b && /^\t+newVideoVirt = ml_io_map_unmappable\(/) { $_ .= "\t\t\t\tdf_mark(12);\n"; $b = 1; }
' "$vc_c"
test "$(grep -c 'df_mark(1[1-2]);' "$vc_c")" -eq 2 || { echo "fix-xcode16: expected df_mark 11-12 in video_console.c"; grep -n df_mark "$vc_c"; exit 1; }
#        11 bands: hang inside ml_io_map_unmappable. Mark 13 = any panic (panic_trap_to_debugger entry),
#        to tell a silent panic from a real hang.
#        Bands 11 + 13 (panic): pmap_map_bd "no PTE" for the early framebuffer io_map -- arm_vm_init's
#        pre-built L3 tables past dynamic_memory_begin ran out. Pre-build 64MB more.
#        The panic printer showed the real cause: "Undefined kernel instruction" on tlbi rvale1is.
#        Range TLBI is ARMv8.4; the Cortex-A55 is v8.2. VMAPPLE.h turns it on unconditionally.
perl -pi -e 's|^#define __ARM_RANGE_TLBI__\s+1$|/* fix-xcode16: Cortex-A55 (v8.2) has no range TLBI */|' "$src/pexpert/pexpert/arm64/VMAPPLE.h"
! grep -q '__ARM_RANGE_TLBI__' "$src/pexpert/pexpert/arm64/VMAPPLE.h" || { echo "fix-xcode16: range TLBI still on"; exit 1; }
#        pmap_switch's mixed-page-size commpage flush uses range TLBI without the guard: flush the core TLB.
perl -0pi -e 's|\t\tconst uint64_t commpage_shift = cpu_data_ptr->commpage_page_shift;\n\t\tconst uint64_t rtlbi_param = generate_rtlbi_param\([^;]*;\n\t\tflush_core_tlb_allrange_async\(rtlbi_param\);\n|\t\tflush_core_tlb_async(); /* fix-xcode16: no range TLBI on Cortex-A55 */\n|' "$src/osfmk/arm/pmap/pmap.c"
grep -q 'no range TLBI on Cortex-A55' "$src/osfmk/arm/pmap/pmap.c" || { echo "fix-xcode16: commpage flush patch failed"; exit 1; }
#        Next panic: "Undefined kernel instruction" on msr DIT (cswitch.s, under __ARM_ARCH_8_4__).
#        VMAPPLE builds with -march=armv8.5a, so the compiler emits v8.4/8.5-only code; the A55 is v8.2.
perl -pi -e 's|^(MACHINE_FLAGS_ARM64_VMAPPLE = -DARM64_BOARD_CONFIG_VMAPPLE\s+)-march=armv8\.5a|$1-march=armv8.2-a|' "$src/makedefs/MakeInc.def"
grep -q 'BOARD_CONFIG_VMAPPLE *-march=armv8.2-a' "$src/makedefs/MakeInc.def" || { echo "fix-xcode16: VMAPPLE -march not lowered"; grep -n VMAPPLE "$src/makedefs/MakeInc.def"; exit 1; }
#        Next panic: NULL g_crypto_funcs in kmem_crypto_init (EARLY_BOOT). On macOS the closed
#        corecrypto.kext registers those functions; this kernel boots without kexts. Register our own
#        minimal RNG for the four random_* entries kmem uses (the others stay NULL until something needs them).
cat >> "$src/libkern/crypto/register_crypto.c" <<'EOF'

/*
 * fix-xcode16 (fire bring-up): no corecrypto.kext, so provide the random_* entries kmem needs at
 * EARLY_BOOT. shortcut: xorshift64 seeded from early_random(); kmem's generator is documented as
 * non-cryptographic, but random_generate/uniform are not: replace with a real DRBG before relying on them.
 */
#include <kern/startup.h>
#include <string.h>
extern uint64_t early_random(void);

static void
df_rng_generate(crypto_random_ctx_t ctx, void *random, size_t random_size)
{
	uint64_t *state = ctx;
	uint8_t *out = random;
	while (random_size) {
		uint64_t x = *state;
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		*state = x;
		size_t n = random_size < sizeof(x) ? random_size : sizeof(x);
		memcpy(out, &x, n);
		out += n;
		random_size -= n;
	}
}

static void
df_rng_uniform(crypto_random_ctx_t ctx, uint64_t bound, uint64_t *random)
{
	uint64_t x;
	df_rng_generate(ctx, &x, sizeof(x));
	*random = bound ? x % bound : 0;
}

static size_t
df_rng_kmem_ctx_size(void)
{
	return sizeof(uint64_t);
}

static void
df_rng_kmem_init(crypto_random_ctx_t ctx)
{
	*(uint64_t *)ctx = early_random() | 1;   /* xorshift state must be non-zero */
}

static struct crypto_functions df_crypto_funcs = {
	.random_generate_fn = df_rng_generate,
	.random_uniform_fn = df_rng_uniform,
	.random_kmem_ctx_size_fn = df_rng_kmem_ctx_size,
	.random_kmem_init_fn = df_rng_kmem_init,
};

static void
df_crypto_register(void)
{
	register_crypto_functions(&df_crypto_funcs);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, df_crypto_register);
EOF
echo "fix-xcode16: registered a minimal kmem RNG in place of corecrypto.kext"

#        Next panic: instruction fetch at 0 from read_random(): the kernel PRNG (cckprng) is also
#        registered by corecrypto.kext via register_and_init_prng(). Install our own on first use.
prng_c="$src/osfmk/prng/prng_random.c"
perl -0pi -e 's|(static SECURITY_READ_ONLY_LATE\(int\) prng_ready;\n)|$1static void df_prng_install(void); /* fix-xcode16 */\n|' "$prng_c"
perl -0pi -e 's|(\nread_random\(void \* buffer, u_int numbytes\)\n\{\n)|$1\tif (!prng_ready) {\n\t\tdf_prng_install(); /* fix-xcode16 */\n\t}\n|; s|(\nwrite_random\(void \* buffer, u_int numbytes\)\n\{\n)|$1\tif (!prng_ready) {\n\t\tdf_prng_install(); /* fix-xcode16 */\n\t}\n|' "$prng_c"
cat >> "$prng_c" <<'EOF'

/*
 * fix-xcode16 (fire bring-up): no corecrypto.kext, so no cckprng. Minimal per-CPU generator
 * registered through the normal register_and_init_prng() path on first use.
 * shortcut: xorshift64* seeded from bootseed + timebase is NOT cryptographically secure;
 * replace with a real DRBG (e.g. HMAC-DRBG over the kernel's SHA256) before trusting /dev/random.
 */
static uint64_t df_prng_state[MAX_CPUS];
static struct cckprng_ctx df_prng_ctx;

static uint64_t
df_prng_mix(const void *p, size_t n, uint64_t h)
{
	const uint8_t *b = p;
	for (size_t i = 0; i < n; i++) {
		h = (h ^ b[i]) * 0x100000001b3ULL;   /* FNV-1a */
	}
	return h;
}

static void
df_prng_init_with_getentropy(struct cckprng_ctx *ctx __unused, unsigned max_ngens __unused, size_t seed_nbytes,
    const void *seed, size_t nonce_nbytes, const void *nonce, cckprng_getentropy getentropy __unused,
    void *getentropy_arg __unused)
{
	uint64_t h = df_prng_mix(seed, seed_nbytes, 0xcbf29ce484222325ULL);
	h = df_prng_mix(nonce, nonce_nbytes, h);
	for (unsigned i = 0; i < MAX_CPUS; i++) {
		df_prng_state[i] = (h ^ (0x9e3779b97f4a7c15ULL * (i + 1))) | 1;
	}
}

static void
df_prng_initgen(struct cckprng_ctx *ctx __unused, unsigned gen_idx __unused)
{
}

static void
df_prng_reseed(struct cckprng_ctx *ctx __unused, size_t nbytes, const void *seed)
{
	for (unsigned i = 0; i < MAX_CPUS; i++) {
		df_prng_state[i] = df_prng_mix(seed, nbytes, df_prng_state[i]) | 1;
	}
}

static void
df_prng_refresh(struct cckprng_ctx *ctx __unused)
{
}

static void
df_prng_generate(struct cckprng_ctx *ctx __unused, unsigned gen_idx, size_t nbytes, void *out)
{
	uint64_t *s = &df_prng_state[gen_idx < MAX_CPUS ? gen_idx : 0];
	uint8_t *o = out;
	while (nbytes) {
		uint64_t x = *s;
		x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
		*s = x;
		x *= 0x2545F4914F6CDD1DULL;   /* xorshift64* */
		size_t n = nbytes < sizeof(x) ? nbytes : sizeof(x);
		memcpy(o, &x, n);
		o += n;
		nbytes -= n;
	}
}

static const struct cckprng_funcs df_prng_funcs = {
	.initgen = df_prng_initgen,
	.reseed = df_prng_reseed,
	.refresh = df_prng_refresh,
	.generate = df_prng_generate,
	.init_with_getentropy = df_prng_init_with_getentropy,
};

static void
df_prng_install(void)
{
	register_and_init_prng(&df_prng_ctx, &df_prng_funcs);
}
EOF
test "$(grep -c 'df_prng_install(); /\* fix-xcode16 \*/' "$prng_c")" -eq 2 || { echo "fix-xcode16: prng install hooks missing"; grep -n df_prng_install "$prng_c"; exit 1; }
echo "fix-xcode16: installed a minimal kernel PRNG in place of cckprng"

#        Next panic: "image4 interface not available" (kern_trustcache.c, kernel_bootstrap_thread).
#        img4if/amfi come from the closed AppleImage4/AMFI kexts. Our userland is open source and
#        unsigned, so run without trust caches: leave the runtime NULL and make every entry point
#        report "not found / not supported" instead of dereferencing it.
tc_c="$src/bsd/kern/kern_trustcache.c"
perl -0pi -e 's|(\tallow_legacy_caches = true;\n#endif\n\n)\t/\* Image4 interface needs to be available \*/\n\tif \(img4if == NULL\) \{\n\t\tpanic\("image4 interface not available"\);\n\t\}\n|$1\t/* fix-xcode16: no AppleImage4/AMFI kexts on fire: run without trust caches */\n\tif (img4if == NULL \|\| amfi == NULL) {\n\t\ttrust_cache_rt = NULL;\n\t\tlck_rw_init(&trust_cache_rt_lock, &trust_cache_lck_grp, 0);\n\t\treturn;\n\t}\n|' "$tc_c"
perl -0pi -e '
  s|(\ncheck_trust_cache_runtime_for_uuid\(\n[^{]*\{\n\tkern_return_t ret = KERN_DENIED;\n)|$1\n\tif (trust_cache_rt == NULL) {\n\t\treturn KERN_NOT_SUPPORTED; /* fix-xcode16 */\n\t}\n|;
  s|(\nload_trust_cache_with_type\(\n[^{]*\{\n)|$1\tif (trust_cache_rt == NULL) {\n\t\treturn KERN_NOT_SUPPORTED; /* fix-xcode16 */\n\t}\n|;
  s|(\nload_legacy_trust_cache\(\n[^{]*\{\n)|$1\tif (trust_cache_rt == NULL) {\n\t\treturn KERN_NOT_SUPPORTED; /* fix-xcode16 */\n\t}\n|;
  s|(\nquery_trust_cache\(\n[^{]*\{\n)|$1\tif (trust_cache_rt == NULL) {\n\t\treturn KERN_NOT_FOUND; /* fix-xcode16 */\n\t}\n|;
' "$tc_c"
grep -q 'run without trust caches' "$tc_c" || { echo "fix-xcode16: trust cache init not patched"; exit 1; }
test "$(grep -c 'trust_cache_rt == NULL) {' "$tc_c")" -eq 5 || { echo "fix-xcode16: expected 4 trust cache NULL guards + load_static"; grep -n 'trust_cache_rt == NULL' "$tc_c"; exit 1; }
echo "fix-xcode16: trust caches disabled (no AppleImage4/AMFI)"

#        Next panic: undefined "mrs x4, SSBS" in Switch_context. The A55 reports ID_AA64PFR1.SSBS = 1
#        (PSTATE bit only); the MRS/MSR SSBS register needs FEAT_SSBS2 (= 2), but commpage.c sets
#        gARM_FEAT_SSBS from >= 1. Require 2, which is what cswitch.s actually uses.
cp_c="$src/osfmk/arm/commpage/commpage.c"
perl -pi -e 's|if \(\(pfr1 & ID_AA64PFR1_EL1_SSBS_MASK\) >= ID_AA64PFR1_EL1_SSBS_EN\) \{|if ((pfr1 & ID_AA64PFR1_EL1_SSBS_MASK) >= (2ull << ID_AA64PFR1_EL1_SSBS_OFFSET)) { /* fix-xcode16: MRS SSBS needs FEAT_SSBS2 */|' "$cp_c"
grep -q 'MRS SSBS needs FEAT_SSBS2' "$cp_c" || { echo "fix-xcode16: SSBS2 check not patched"; exit 1; }
avi_c="$(dirname "$arm_init")/../arm64/arm_vm_init.c"
perl -pi -e 's|^(\tva_l1_end = va_l1 \+ \(\(2 \+ \(mem_segments \* 10\))\) << 20\);|$1 + 64) << 20); /* fix-xcode16: room for early io_map */|' "$avi_c"
grep -q 'room for early io_map' "$avi_c" || { echo "fix-xcode16: L3 prealloc patch failed"; exit 1; }
#        With /arm-io/gic in the ADT, fire hangs silently (no panic) in pe_init_fiq. Marks 14-18 bisect it:
#        14 after GICD/GICR io_map, 15 after GICR_WAKER wake, 16 after ICC_SRE, 17 after BPR0/PMR/CTLR,
#        18 at the end (after GICD_CTLR + ICC_IGRPEN0).
fiq_c="$src/pexpert/arm/pe_fiq.c"
perl -0pi -e 's|^|extern void df_mark(int); /* fix-xcode16: fire bring-up */\n|' "$fiq_c"
perl -pi -e '
  if (!$a && /^\t\tgicr_size = gicr_size_prop;/) { $_ .= "\t\tdf_mark(14);\n"; $a = 1; }
  elsif (!$b && m{^\t// Configure timers and legacy FIQ to be group 0}) { $_ = "\tdf_mark(15);\n$_"; $b = 1; }
  elsif (!$c && m{^\t// Set priority masks and binary point for group 0}) { $_ = "\tdf_mark(16);\n$_"; $c = 1; }
  elsif (!$d && m{^\t// Enable the forwarding of the vtimer interrupt}) { $_ = "\tdf_mark(17);\n$_"; $d = 1; }
  elsif (!$e && /^\t__builtin_arm_wsr64\("ICC_IGRPEN0_EL1", 1\);/) { $_ .= "\tdf_mark(18);\n"; $e = 1; }
' "$fiq_c"
test "$(grep -c 'df_mark(1[4-8]);' "$fiq_c")" -eq 5 || { echo "fix-xcode16: expected df_mark 14-18 in pe_fiq.c"; grep -n df_mark "$fiq_c"; exit 1; }
#        Fire reached mark 16-17 and hung: the Group 0 (FIQ) setup. On MediaTek the secure firmware owns
#        Group 0; a Non-secure EL1 kernel (Linux on this phone) only uses Group 1. Use Group 1 instead:
#        no IGROUPR0 write (NS can't regroup), BPR1/IGRPEN1 instead of BPR0/IGRPEN0, leave GICD_CTLR alone.
perl -0pi -e '
  s|\tgicr_write32\(GICR_IGROUPR0, 0x81FFFFFF\);\n|\t/* fix-xcode16: NS EL1 cannot regroup; PPIs stay Group 1 NS */\n|;
  s|"ICC_BPR0_EL1"|"ICC_BPR1_EL1"|;
  s#\tuint32_t gicd_ctlr = gicd_read32\(GICD_CTLR\);\n\tgicd_ctlr \|= GICD_CTLR_ENABLEGRP0;\n\tgicd_write32\(GICD_CTLR, gicd_ctlr\);\n#\t/* fix-xcode16: GICD_CTLR is configured by the secure firmware */\n#;
  s|"ICC_IGRPEN0_EL1"|"ICC_IGRPEN1_EL1"|;
' "$fiq_c"
for want in 'NS EL1 cannot regroup' 'ICC_BPR1_EL1' 'configured by the secure firmware' 'ICC_IGRPEN1_EL1'; do
	grep -q "$want" "$fiq_c" || { echo "fix-xcode16: pe_fiq Group 1 patch missing: $want"; exit 1; }
done
! grep -q 'GICD_CTLR_ENABLEGRP0\|ICC_IGRPEN0_EL1\|0x81FFFFFF' "$fiq_c" || { echo "fix-xcode16: pe_fiq still has Group 0 setup"; grep -n 'GRP0\|IGRPEN0\|0x81FF' "$fiq_c"; exit 1; }
#        Next panic: kalloc_type_create_zone_for_size wrote entry 126 of its 32-entry stack array; the zone
#        count (x25) was 0xfe10, a uint16 underflow from kalloc_type_apply_policy. Panic with the whole
#        per-sizeclass table (size:unique_sig/total_types/zones_sig/zones_type) to see where it goes wrong.
kalloc_c="$src/osfmk/kern/kalloc.c"
#        The table showed size 32 gets 7+25 = 32 zones: fits the array, so the policy is fine. Only flag a
#        real overflow (> 32), and trap inside the creation loop itself: if i passes 32 while kt_zones
#        says 32, a callee is clobbering the loop's callee-saved registers.
perl -0pi -e 's|(\tfor \(uint16_t i = 0; i < kt_zones; i\+\+\) \{\n\t\tz_name = zalloc_permanent)|\tfor (uint16_t i = 0; i < kt_zones; i++) {\n\t\tif (i >= 32) { panic("fire kt loop overflow: i=%u kt_zones=%u z_size=%lu", i, kt_zones, (unsigned long)z_size); } /* fix-xcode16 */\n\t\tz_name = zalloc_permanent|' "$kalloc_c"
grep -q 'fire kt loop overflow' "$kalloc_c" || { echo "fix-xcode16: kalloc_type loop check not inserted"; exit 1; }
#        Root cause found: the garbage 65535/65024/1939/33472 in kt_zones_type is a zone pointer
#        (0xfffffe00079382c0). Size 32 got 7+25 = 32 zones, but the shared zone needs one more slot
#        (assert n+1 <= 32, compiled out in RELEASE): kt_zones_for_size[32] overflowed into the tables.
#        Clamp every size class to 31 zones, taking the excess from the type zones first.
perl -0pi -e 's|(\t    kt_zones_sig, kt_zones_type, kt_fixed_zones\);\n)|$1\tfor (uint16_t j = 0; j < MAX_K_ZONE(kt_zone_cfg); j++) { /* fix-xcode16: leave room for the shared zone */\n\t\twhile (kt_zones_sig[j] + kt_zones_type[j] + 1 > 32) {\n\t\t\tif (kt_zones_type[j]) { kt_zones_type[j]--; } else { kt_zones_sig[j]--; }\n\t\t}\n\t}\n|' "$kalloc_c"
grep -q 'leave room for the shared zone' "$kalloc_c" || { echo "fix-xcode16: kalloc_type clamp not inserted"; exit 1; }
perl -0pi -e 's|(\t\tassert\(n_zones_sig \+ n_zones_type \+ 1 <= 32\);\n)|\t\tif (n_zones_sig + n_zones_type > 32) { /* fix-xcode16: fire diagnostics */\n\t\t\tstatic char df_kt[1536]; int df_o = 0;\n\t\t\tfor (uint16_t j = 0; j < MAX_K_ZONE(kt_zone_cfg) && df_o < (int)sizeof(df_kt) - 40; j++) {\n\t\t\t\tdf_o += snprintf(df_kt + df_o, sizeof(df_kt) - df_o, "%u:%u/%u/%u/%u ", kt_zone_cfg[j], kt_freq_list[j], kt_freq_list_total[j], kt_zones_sig[j], kt_zones_type[j]);\n\t\t\t}\n\t\t\tpanic("fire kt i=%u budget=%u: %s", i, kt_fixed_zones, df_kt);\n\t\t}\n$1|' "$kalloc_c"
grep -q 'fire kt i=' "$kalloc_c" || { echo "fix-xcode16: kalloc_type diagnostic not inserted"; exit 1; }
#        Next panic: an IRQ during early boot reached PE_handle_ext_interrupt -> gCPUIC (IOKit's interrupt
#        controller, not created yet) == NULL. On VMAPPLE the timer is a Group 0 FIQ; on MediaTek a
#        Non-secure kernel gets it as a Group 1 IRQ (LK already left Group 1 enabled). Ack the GIC in
#        sleh_irq: timer PPIs (27 virtual, 30 physical) go to rtclock_intr exactly as sleh_fiq does.
sleh_c="$src/osfmk/arm64/sleh.c"
perl -0pi -e 's|(#if USE_APPLEARMSMP\n)\tPE_handle_ext_interrupt\(\);\n#else|$1\t{ /* fix-xcode16: fire GICv3 Group 1 IRQs */\n\t\tuint64_t df_iar = __builtin_arm_rsr64("ICC_IAR1_EL1");\n\t\tuint32_t df_id = (uint32_t)(df_iar & 0xffffff);\n\t\tif (df_id == 27 \|\| df_id == 30) {\n\t\t\tcdp->cpu_decrementer = -1; /* Large */\n\t\t\trtclock_intr(TRUE);\n\t\t}\n\t\t/* shortcut: device IRQs are acked and dropped until an IOKit GIC driver exists */\n\t\tif (df_id < 1020) {\n\t\t\t__builtin_arm_wsr64("ICC_EOIR1_EL1", df_iar);\n\t\t\t__builtin_arm_isb(ISB_SY);\n\t\t}\n\t}\n#else|' "$sleh_c"
grep -q 'fire GICv3 Group 1 IRQs' "$sleh_c" || { echo "fix-xcode16: sleh_irq GIC path not inserted"; grep -n -A3 'USE_APPLEARMSMP' "$sleh_c"; exit 1; }
dbg_c="$(dirname "$arm_init")/../kern/debug.c"
#        Still 11 + 13 after the L3 prealloc: print the panic string itself at the screen bottom.
perl -0pi -e 's|^|extern void df_mark(int); extern void df_puts(const char *, unsigned long); /* fix-xcode16: fire bring-up */\n|; s|(\npanic_trap_to_debugger\([^{]*\{\n#pragma clang diagnostic pop\n)|$1\tdf_mark(13);\n\t{ extern int vsnprintf(char *, size_t, const char *, va_list); static char df_b[3072]; strlcpy(df_b, "PANIC: ", sizeof(df_b)); if (panic_args) { va_list df_c; va_copy(df_c, *panic_args); vsnprintf(df_b + 7, sizeof(df_b) - 7, panic_format_str, df_c); va_end(df_c); } else { strlcpy(df_b + 7, panic_format_str, sizeof(df_b) - 7); } df_puts(df_b, panic_caller); }\n|' "$dbg_c"
grep -q 'df_mark(13);' "$dbg_c" || { echo "fix-xcode16: expected df_mark 13 in debug.c"; exit 1; }
echo "fix-xcode16: kept V=P after arm_vm_init, df_mark 9-12"

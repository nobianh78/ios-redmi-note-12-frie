// bootimg_builder.js prepends the 64-byte ARM64 Linux header, so link offset 0 sits at
// load address + 0x40 (load address 0x40080000 is 2 MB aligned). An in-image header was tried and
// fire's LK never jumped to it (bootloop, 2026-11-12), so keep the builder's header.
// LK jumps here with x0 = FDT, MMU and D-cache off.
.section .text.start, "ax"
.global _start
_start:
    // First thing: disable the MTK TOPRGU watchdog LK leaves armed (WDT_MODE = key 0x22000000, EN=0).
    // If the phone then never self-resets, this instruction ran. Nothing above it can fail.
    mov  x2, #0x7000
    movk x2, #0x1000, lsl #16      // 0x10007000
    mov  w3, #0x22000000
    str  w3, [x2]
    mov  x19, x0
    adr  x1, stack_top          // PC-relative: image is loaded at an unknown offset
    mov  sp, x1
    mov  x0, x19
    bl   heartbeat_main
1:  wfe
    b    1b

// void xnu_jump(uint64_t entry_phys, uint64_t boot_args_phys)
// XNU's start expects x0 = boot_args (physical), MMU off, interrupts masked, at EL1.
.text
.global xnu_jump
xnu_jump:
    msr  daifset, #0xf
    mov  x16, x0
    mov  x0, x1
    mov  x1, xzr
    mov  x2, xzr
    mov  x3, xzr
    // Proof we reached the branch: grey band at rows 1200..1240 (XNU's DF_MARK bands start at row 1300).
    // Must target OVL layer 3 (0x7ce6a000, pitch 4320): it sits on top of layer 0 and hides it.
    ldr  x9, =(0x7ce6a000 + 1200 * 4320)
    mov  x10, #((40 * 4320) / 4)
    ldr  w11, =0xff808080
1:  str  w11, [x9], #4
    sub  x10, x10, #1
    cbnz x10, 1b
    // LK ran with caches on: drop any stale instruction-cache lines / TLB entries before running
    // freshly decompressed code.
    dsb  sy
    ic   iallu
    tlbi vmalle1
    dsb  sy
    isb
    br   x16
    .ltorg

// Exception vectors: any fault before XNU installs its own VBAR lands here and is printed on screen.
// Each slot reloads our own stack (the faulting context's SP is not trusted) and reports.
// Each slot is exactly 0x80 bytes (9 instructions + padding); no .balign, which would undo the 0x7c0 skew.
.macro VEC idx
    adr  x9, exc_stack_top
    mov  sp, x9
    mov  x0, #\idx
    mrs  x1, esr_el1
    mrs  x2, elr_el1
    mrs  x3, far_el1
    bl   exc_report
2:  wfe
    b    2b
    .space 0x80 - 9 * 4
.endm

// VBAR_EL1 needs a 2 KB aligned *physical* address; link offset X runs at load+0x40+X,
// so X must be 0x7c0 mod 0x800. heartbeat.c checks the runtime alignment before using it.
.balign 0x800
.space 0x7c0
.global exc_vectors
exc_vectors:
    VEC 0
    VEC 1
    VEC 2
    VEC 3
    VEC 4
    VEC 5
    VEC 6
    VEC 7
    VEC 8
    VEC 9
    VEC 10
    VEC 11
    VEC 12
    VEC 13
    VEC 14
    VEC 15

.section .data.stack, "aw"
.balign 16
.space 8192
exc_stack_top:

// Stack lives in .data so objcopy -O binary includes it and image_size covers it.
.section .data.stack, "aw"
.balign 16
.space 16384
stack_top:

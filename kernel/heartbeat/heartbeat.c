/*
 * DarwinFire heartbeat: first code running on the Redmi 12 ("fire").
 * Prints a diagnostic text screen (FDT framebuffer, OVL layer state, exception level, uptime)
 * to every candidate scanout buffer, then stays alive with a ticking counter.
 * Test method (see memory/notes): flash to boot_b + set-active b; `fastboot boot` does not work on fire.
 */
#include <stdint.h>
#include "fbcon.h"

#define FDT_MAGIC      0xd00dfeedu
#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4

#define MAX_FB_SIZE    (64ull << 20)           /* never scribble over more than 64 MB */

static uint32_t be32(const uint8_t *b) {
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static int starts_with(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

/* fire's LK names it "reserved-memory/mblock-17-framebuffer", so match anywhere in the name. */
static int contains(const char *s, const char *p) {
    for (; *s; s++) if (starts_with(s, p)) return 1;
    return 0;
}

static int str_eq_n(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Find property `prop` in the first node whose name contains `node`; returns its value pointer and length. */
static const uint8_t *fdt_prop(const uint8_t *fdt, const char *node, const char *prop, uint32_t *len_out) {
    if (be32(fdt) != FDT_MAGIC) return 0;
    const uint8_t *p = fdt + be32(fdt + 8);
    const char *strs = (const char *)fdt + be32(fdt + 12);
    const uint8_t *end = p + be32(fdt + 36);
    int depth = 0, hit = -1;

    while (p < end) {
        uint32_t tok = be32(p);
        p += 4;
        if (tok == FDT_BEGIN_NODE) {
            depth++;
            if (hit < 0 && contains((const char *)p, node)) hit = depth;
            while (*p) p++;
            p = fdt + ((p + 1 - fdt + 3) & ~3u);
        } else if (tok == FDT_END_NODE) {
            if (depth == hit) hit = -1;
            depth--;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(p), nameoff = be32(p + 4);
            const uint8_t *v = p + 8;
            if (depth == hit && str_eq(strs + nameoff, prop)) { *len_out = len; return v; }
            p = v + ((len + 3) & ~3u);
        } else if (tok != FDT_NOP) {
            return 0;   /* FDT_END or corrupt */
        }
    }
    return 0;
}

/* 1- or 2-cell big-endian integer property (e.g. /chosen/linux,initrd-start). */
int fdt_u64(const uint8_t *fdt, const char *node, const char *prop, uint64_t *out) {
    uint32_t len = 0;
    const uint8_t *v = fdt_prop(fdt, node, prop, &len);
    if (!v || (len != 4 && len != 8)) return 0;
    *out = len == 8 ? ((uint64_t)be32(v) << 32 | be32(v + 4)) : be32(v);
    return 1;
}

/* Find the first node whose name contains "framebuffer" and read its reg.
 * shortcut: assumes #address-cells = #size-cells = 2 (true on arm64 MTK DTs); read parent cells if a device differs. */
int find_fb(const uint8_t *fdt, uint64_t *base, uint64_t *size) {
    uint32_t len = 0;
    const uint8_t *v = fdt_prop(fdt, "framebuffer", "reg", &len);
    if (!v || len < 16) return 0;
    *base = (uint64_t)be32(v) << 32 | be32(v + 4);
    *size = (uint64_t)be32(v + 8) << 32 | be32(v + 12);
    return 1;
}

#ifndef HOST_TEST   /* hardware-only below; test_find_fb.c checks the parser and console on the PC */
static uint64_t cntfrq(void) {
    uint64_t frq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    return frq ? frq : 13000000;   /* MT6768 system counter is 13 MHz if LK left CNTFRQ unset */
}

static uint64_t cntvct(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static uint32_t current_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (uint32_t)(el >> 2) & 3;
}

/* fire's panel is DSC video mode (LCM_name=..._dsc_vdo_...), so it scans continuously from whatever
 * buffers the OVL layers point at. Offsets from MTK ddp_reg_ovl.h (mt6765/mt6768 family). */
#define OVL_SRC_CON     0x2c
#define OVL_L0_SRC_SIZE 0x38   /* height << 16 | width */
#define OVL_L0_PITCH    0x44   /* bytes per line in [15:0] */
#define OVL_L0_ADDR     0xf40
#define OVL_LAYER_STEP  0x20
#define DRAM_BASE       0x40000000ull

struct layer { uint64_t addr; uint32_t pitch, w, h; int ok; };

static uint32_t read_ovl(uint64_t ovl, struct layer out[4]) {
    volatile uint32_t *r = (volatile uint32_t *)ovl;
    uint32_t en = r[OVL_SRC_CON / 4];
    for (int i = 0; i < 4; i++) {
        uint32_t size = r[(OVL_L0_SRC_SIZE + i * OVL_LAYER_STEP) / 4];
        out[i].addr  = r[(OVL_L0_ADDR + i * OVL_LAYER_STEP) / 4];
        out[i].pitch = r[(OVL_L0_PITCH + i * OVL_LAYER_STEP) / 4] & 0xffff;
        out[i].w     = size & 0x1fff;
        out[i].h     = (size >> 16) & 0x1fff;
        /* only draw into enabled layers that point at plausible DRAM buffers */
        out[i].ok = (en & (1u << i)) && out[i].addr >= DRAM_BASE && out[i].pitch && out[i].w && out[i].h &&
                    (uint64_t)out[i].pitch * out[i].h <= MAX_FB_SIZE;
    }
    return en;
}

static void print_layers(const char *name, uint32_t en, const struct layer l[4]) {
    fbcon_puts(name); fbcon_puts(" en="); fbcon_hex(en); fbcon_putc('\n');
    for (int i = 0; i < 4; i++) {
        if (!(en & (1u << i))) continue;
        fbcon_puts(" L"); fbcon_dec(i); fbcon_puts(" "); fbcon_hex(l[i].addr);
        fbcon_puts(" p="); fbcon_dec(l[i].pitch);
        fbcon_puts(" "); fbcon_dec(l[i].w); fbcon_putc('x'); fbcon_dec(l[i].h);
        fbcon_puts(l[i].ok ? "\n" : " skip\n");
    }
}

/* Ticking counter proves the CPU and timer keep running after we print. */
static void idle_forever(uint32_t row) {
    uint64_t frq = cntfrq(), t0 = cntvct();
    for (uint64_t last = ~0ull;;) {
        uint64_t s = (cntvct() - t0) / frq;
        if (s != last) {
            fbcon_setpos(0, row);
            fbcon_puts("alive: "); fbcon_dec(s); fbcon_puts(" s   ");
            __asm__ volatile("dsb sy" ::: "memory");
            last = s;
        }
    }
}

/* ---- XNU loader -------------------------------------------------------------------------------- */
#include "../include/mach/boot_args.h"

/* Header written by tools/xnu_flatten.js pack(). */
struct xnu_payload {
    char     magic[8];        /* "XNUFLAT1" */
    uint64_t virt_base;
    uint64_t entry_offset;
    uint64_t kernel_size;
    uint64_t adt_offset;      /* from start of payload */
    uint64_t adt_size;
};
#define PAYLOAD_HDR   0x4000ull
#define PAGE16K(x)    (((x) + 0x3fffull) & ~0x3fffull)
/* shortcut: XNU gets one contiguous RAM range from physBase up to just below the framebuffer;
 * MTK reserved carve-outs (TEE/ATF/modem) inside it are not excluded yet. Feed them via the ADT once XNU gets that far. */
#define XNU_RAM_END   0x7c000000ull

extern void xnu_jump(uint64_t entry_phys, uint64_t boot_args_phys);
extern char exc_vectors[];
extern char _image_end[];   /* runtime address via adr; the appended XNU payload starts at the next 16K page */

static uint64_t g_phys_base, g_virt_base, g_kernel_size;

/* Called from start.S vectors: something faulted before XNU installed its own vectors. */
void exc_report(uint64_t idx, uint64_t esr, uint64_t elr, uint64_t far) {
    fbcon_setpos(0, 30);
    fbcon_puts("*** EXCEPTION ***\n");
    fbcon_puts("vec: "); fbcon_dec(idx);
    fbcon_puts("  EC: "); fbcon_hex(esr >> 26);
    fbcon_puts("\nESR: "); fbcon_hex(esr);
    fbcon_puts("\nELR: "); fbcon_hex(elr);
    fbcon_puts("\nFAR: "); fbcon_hex(far);
    if (g_kernel_size && elr >= g_phys_base && elr < g_phys_base + g_kernel_size) {
        fbcon_puts("\nkernel VA: "); fbcon_hex(elr - g_phys_base + g_virt_base);   /* look this up in the Mach-O */
    }
    fbcon_putc('\n');
    __asm__ volatile("dsb sy" ::: "memory");
}

static int boot_xnu(uint64_t initrd, const struct fb_surface *video) {
    const struct xnu_payload *p = (const struct xnu_payload *)initrd;
    fbcon_puts("payload @"); fbcon_hex(initrd); fbcon_puts(": ");
    if (!initrd || !str_eq_n(p->magic, "XNUFLAT1", 8)) { fbcon_puts("none\n"); return 0; }
    fbcon_puts("XNUFLAT1\n");
    if (!video->base) { fbcon_puts("no video surface\n"); return 0; }

    uint64_t phys_base = initrd + PAYLOAD_HDR;
    if (phys_base & 0x3fff) { fbcon_puts("payload not 16K aligned\n"); return 0; }
    if (p->entry_offset >= p->kernel_size || p->adt_offset + p->adt_size < p->adt_offset) {
        fbcon_puts("bad payload header\n"); return 0;
    }

    /* boot_args in the 16K page after the ADT; XNU may reuse everything below topOfKernelData. */
    uint64_t adt_phys = initrd + p->adt_offset;
    uint64_t ba_phys  = PAGE16K(adt_phys + p->adt_size);
    darwin_arm64_boot_args_t *ba = (darwin_arm64_boot_args_t *)ba_phys;
    for (uint64_t i = 0; i < sizeof(*ba); i++) ((volatile uint8_t *)ba)[i] = 0;

    ba->Revision         = kBootArgsRevision2;
    ba->Version          = kBootArgsVersion2;
    ba->virtBase         = p->virt_base;
    ba->physBase         = phys_base;
    ba->memSize          = XNU_RAM_END - phys_base;
    ba->topOfKernelData  = PAGE16K(ba_phys + sizeof(*ba));
    ba->video.v_baseAddr = video->base;
    ba->video.v_display  = 1;
    ba->video.v_rowBytes = video->pitch;
    ba->video.v_width    = video->width;
    ba->video.v_height   = video->height;
    ba->video.v_depth    = 32;
    ba->deviceTreeP      = adt_phys - phys_base + p->virt_base;   /* XNU reads the ADT through its kernel VA */
    ba->deviceTreeLength = (uint32_t)p->adt_size;
    const char *cmd = "-v";   /* verbose console text on screen */
    for (int i = 0; cmd[i] && i < BOOT_LINE_LENGTH - 1; i++) ba->CommandLine[i] = cmd[i];

    g_phys_base = phys_base; g_virt_base = p->virt_base; g_kernel_size = p->kernel_size;
    uint64_t entry = phys_base + p->entry_offset;

    /* Self-built XNU drew no breadcrumb band: check what is really in RAM at the entry point (LK may
     * truncate a large decompressed kernel) and that the first instruction XNU runs is allowed here. */
    fbcon_puts("entry word: "); fbcon_hex(*(volatile uint32_t *)(phys_base + p->entry_offset));
    /* shortcut: offset 0xb31a64 is the last non-zero word of this particular build (expect 0x6f72); drop after the check */
    if (p->kernel_size > 0xb31a68) {
        fbcon_puts("\nword@b31a64: "); fbcon_hex(*(volatile uint32_t *)(phys_base + 0xb31a64));
    }
    fbcon_puts("\nOSLAR test... ");
    __asm__ volatile("dsb sy" ::: "memory");
    __asm__ volatile("msr oslar_el1, xzr; isb");
    fbcon_puts("ok\n");

    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    fbcon_puts("XNU physBase: "); fbcon_hex(phys_base);
    fbcon_puts("\nentry: "); fbcon_hex(entry);
    fbcon_puts("\nboot_args: "); fbcon_hex(ba_phys);
    fbcon_puts("\nSCTLR_EL1: "); fbcon_hex(sctlr);
    fbcon_puts("\njumping to XNU...\n");
    __asm__ volatile("dsb sy" ::: "memory");

    if (((uint64_t)exc_vectors & 0x7ff) == 0) __asm__ volatile("msr vbar_el1, %0; isb" :: "r"(exc_vectors));
    else { fbcon_puts("vectors misaligned: "); fbcon_hex((uint64_t)exc_vectors); fbcon_puts(" (no fault report)\n"); }
    xnu_jump(entry, ba_phys);
    return 1;   /* not reached */
}

void heartbeat_main(uint64_t fdt_addr) {
    struct layer ovl0[4], ovl2l[4];
    uint32_t en0 = read_ovl(0x1400b000ull, ovl0);
    uint32_t en2 = read_ovl(0x1400c000ull, ovl2l);
    for (int i = 0; i < 4; i++) {
        if (ovl0[i].ok)  fbcon_add(ovl0[i].addr,  ovl0[i].pitch,  ovl0[i].w,  ovl0[i].h,  ovl0[i].pitch / ovl0[i].w);
        if (ovl2l[i].ok) fbcon_add(ovl2l[i].addr, ovl2l[i].pitch, ovl2l[i].w, ovl2l[i].h, ovl2l[i].pitch / ovl2l[i].w);
    }

    uint64_t fb = 0, fb_size = 0;
    int have_fb = find_fb((const uint8_t *)fdt_addr, &fb, &fb_size);
    /* shortcut: FDT gives no geometry, so assume the panel's 1080x2460 ARGB; only used if OVL gave nothing */
    if (!fbcon_count() && have_fb && fb_size >= 1080ull * 2460 * 4) fbcon_add(fb, 1080 * 4, 1080, 2460, 4);

    fbcon_clear(0xFF102040);
    fbcon_puts("\n\n");   /* skip rows hidden by the rounded corners / camera notch */
    fbcon_puts("DarwinFire heartbeat\n");
    fbcon_puts("Redmi 12 fire / MT6768\n\n");
    fbcon_puts("EL: "); fbcon_dec(current_el()); fbcon_putc('\n');
    fbcon_puts("FDT: "); fbcon_hex(fdt_addr); fbcon_putc('\n');
    fbcon_puts("FDT fb: ");
    if (have_fb) { fbcon_hex(fb); fbcon_puts(" +"); fbcon_hex(fb_size); }
    else fbcon_puts("not found");
    fbcon_puts("\ncntfrq: "); fbcon_dec(cntfrq()); fbcon_puts("\n\n");
    print_layers("OVL0", en0, ovl0);
    print_layers("OVL0_2L", en2, ovl2l);
    fbcon_puts("\nsurfaces: "); fbcon_dec(fbcon_count()); fbcon_puts("\n\n");

    /* Video for XNU: the TOPMOST enabled OVL layer. Higher layers cover lower ones; on fire layer 3
     * (0x7ce6a000, pitch 4320) hides layer 0, so writes to layer 0 never reach the panel. */
    struct fb_surface video = { 0 };
    for (int i = 3; i >= 0 && !video.base; i--) {
        if (ovl0[i].ok) video = (struct fb_surface){ ovl0[i].addr, ovl0[i].pitch, ovl0[i].w, ovl0[i].h, 4 };
    }
    if (!video.base && have_fb) video = (struct fb_surface){ fb, 1080 * 4, 1080, 2460, 4 };
    fbcon_puts("video: "); fbcon_hex(video.base); fbcon_puts(" p="); fbcon_dec(video.pitch); fbcon_putc('\n');

    /* XNU payload: appended after this image inside the gzip'd kernel (LK rejects a 49 MB ramdisk),
     * else in the ramdisk. boot_xnu returns only if there is no valid payload. */
    uint64_t initrd = 0;
    fdt_u64((const uint8_t *)fdt_addr, "chosen", "linux,initrd-start", &initrd);
    fbcon_puts("initrd: "); fbcon_hex(initrd); fbcon_putc('\n');
    boot_xnu(PAGE16K((uint64_t)_image_end), &video);
    boot_xnu(initrd, &video);
    idle_forever(40);
}
#endif

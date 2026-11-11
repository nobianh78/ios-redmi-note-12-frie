/* Host self-check for find_fb and fbcon. Build: clang -DHOST_TEST test_find_fb.c heartbeat.c fbcon.c -o t.exe */
/* Note: FDT header field 9 is size_dt_struct (offset 36), used by find_fb as the loop bound. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fbcon.h"

int find_fb(const uint8_t *fdt, uint64_t *base, uint64_t *size);
int fdt_u64(const uint8_t *fdt, const char *node, const char *prop, uint64_t *out);

static uint8_t buf[512];
static size_t n;
static void w32(uint32_t v) { buf[n++] = v >> 24; buf[n++] = v >> 16; buf[n++] = v >> 8; buf[n++] = v; }
static void wstr(const char *s) { size_t l = strlen(s) + 1; memcpy(buf + n, s, l); n += l; while (n & 3) buf[n++] = 0; }

int main(void) {
    /* strings: "compatible\0reg\0" -> reg at offset 11 */
    n = 40;
    size_t struct_off = n;
    w32(1); wstr("");                              /* root */
    w32(1); wstr("chosen");
    w32(3); w32(4); w32(0); w32(0x12345678);       /* compatible (decoy prop) */
    w32(3); w32(8); w32(15); w32(0); w32(0x47c80000); /* linux,initrd-start, 2 cells */
    w32(2);
    w32(1); wstr("mblock-17-framebuffer");   /* real node name on fire */
    w32(3); w32(16); w32(11);                      /* reg */
    w32(0); w32(0x7e000000); w32(0); w32(0x00a23000);
    w32(2);
    w32(2); w32(9);
    size_t struct_size = n - struct_off, str_off = n;
    memcpy(buf + n, "compatible\0reg\0linux,initrd-start\0", 34); n += 34;

    uint32_t hdr[10] = {0xd00dfeed, (uint32_t)n, (uint32_t)struct_off, (uint32_t)str_off, 0, 17, 16, 0, 34, (uint32_t)struct_size};
    for (int i = 0; i < 10; i++) { uint8_t *p = buf + i * 4; p[0] = hdr[i] >> 24; p[1] = hdr[i] >> 16; p[2] = hdr[i] >> 8; p[3] = hdr[i]; }

    uint64_t b = 0, s = 0;
    assert(find_fb(buf, &b, &s) == 1);
    assert(b == 0x7e000000 && s == 0xa23000);
    uint64_t initrd = 0;
    assert(fdt_u64(buf, "chosen", "linux,initrd-start", &initrd) == 1 && initrd == 0x47c80000);
    assert(fdt_u64(buf, "chosen", "missing-prop", &initrd) == 0);
    puts("fdt_u64 OK");
    buf[0] = 0;
    assert(find_fb(buf, &b, &s) == 0);   /* bad magic rejected */
    puts("find_fb OK");

    /* Console: draw "A" into a 64x32 ARGB buffer; glyph row 0 of 'A' is 0x0C -> pixels 2,3 lit (x4 scale). */
    static uint32_t px[64 * 32];
    assert(fbcon_add((uint64_t)(uintptr_t)px, 64 * 4, 64, 32, 4) == 0);
    fbcon_clear(0xFF000000);
    fbcon_putc('A');
    assert(px[0 * 64 + 1 * 4] == 0xFF000000);      /* glyph col 1: off */
    assert(px[0 * 64 + 2 * 4] == 0xFFFFFFFF);      /* glyph col 2: on  */
    assert(px[3 * 64 + 3 * 4 + 3] == 0xFFFFFFFF);  /* scaled block stays lit */
    assert(px[0 * 64 + 4 * 4] == 0xFF000000);      /* glyph col 4: off */
    fbcon_putc('\n'); fbcon_putc('B');             /* row 1 is off-surface: must not write out of bounds */
    puts("fbcon OK");
    return 0;
}

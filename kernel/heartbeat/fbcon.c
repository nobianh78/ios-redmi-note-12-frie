/* Minimal framebuffer text console: 8x8 font scaled up, written to every registered surface
 * (we don't yet know which buffer the panel scans out, so draw to all candidates). */
#include "fbcon.h"
#include "font8x8.h"

#define SCALE    4            /* 32x32 px glyphs: ~33 cols x 76 rows on 1080x2460 */
#define CELL     (8 * SCALE)
#define MAX_SURF 8

static struct fb_surface g_surf[MAX_SURF];
static int g_nsurf;
static uint32_t g_col, g_row;
static uint32_t g_fg = 0xFFFFFFFF, g_bg = 0xFF102040;

static void put_px(const struct fb_surface *s, uint32_t x, uint32_t y, uint32_t argb) {
    if (x >= s->width || y >= s->height) return;
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)(s->base + (uint64_t)y * s->pitch + (uint64_t)x * s->bpp);
    if (s->bpp == 4) {
        *(volatile uint32_t *)p = argb;
    } else if (s->bpp == 2) {   /* RGB565 */
        *(volatile uint16_t *)p = (uint16_t)(((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F));
    } else if (s->bpp == 3) {
        p[0] = (uint8_t)argb; p[1] = (uint8_t)(argb >> 8); p[2] = (uint8_t)(argb >> 16);
    }
}

int fbcon_add(uint64_t base, uint32_t pitch, uint32_t width, uint32_t height, uint32_t bpp) {
    if (g_nsurf >= MAX_SURF || !base || !pitch || !width || !height) return -1;
    if (bpp < 2 || bpp > 4) bpp = 4;
    g_surf[g_nsurf++] = (struct fb_surface){ base, pitch, width, height, bpp };
    return 0;
}

int fbcon_count(void) { return g_nsurf; }

void fbcon_clear(uint32_t argb) {
    g_bg = argb;
    for (int i = 0; i < g_nsurf; i++)
        for (uint32_t y = 0; y < g_surf[i].height; y++)
            for (uint32_t x = 0; x < g_surf[i].width; x++) put_px(&g_surf[i], x, y, argb);
    g_col = g_row = 0;
}

void fbcon_setpos(uint32_t col, uint32_t row) { g_col = col; g_row = row; }

static void draw_glyph(char c) {
    const uint8_t *g = font8x8[(c < 0x20 || c > 0x7E) ? '?' - 0x20 : c - 0x20];
    for (int i = 0; i < g_nsurf; i++) {
        const struct fb_surface *s = &g_surf[i];
        uint32_t ox = g_col * CELL, oy = g_row * CELL;
        for (uint32_t y = 0; y < CELL; y++)
            for (uint32_t x = 0; x < CELL; x++)
                put_px(s, ox + x, oy + y, (g[y / SCALE] >> (x / SCALE)) & 1 ? g_fg : g_bg);
    }
}

void fbcon_putc(char c) {
    uint32_t cols = g_nsurf ? g_surf[0].width / CELL : 1;
    if (c == '\n') { g_col = 0; g_row++; return; }
    if (g_col >= cols) { g_col = 0; g_row++; }
    draw_glyph(c);
    g_col++;
}

void fbcon_puts(const char *s) { while (*s) fbcon_putc(*s++); }

void fbcon_hex(uint64_t v) {
    fbcon_puts("0x");
    int started = 0;
    for (int sh = 60; sh >= 0; sh -= 4) {
        uint32_t d = (v >> sh) & 0xF;
        if (d || started || sh == 0) { fbcon_putc((char)(d < 10 ? '0' + d : 'a' + d - 10)); started = 1; }
    }
}

void fbcon_dec(uint64_t v) {
    char buf[21];
    int n = 0;
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) fbcon_putc(buf[--n]);
}

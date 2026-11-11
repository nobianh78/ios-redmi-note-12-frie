#pragma once
#include <stdint.h>

struct fb_surface {
    uint64_t base;
    uint32_t pitch, width, height, bpp;   /* bpp = bytes per pixel */
};

int  fbcon_add(uint64_t base, uint32_t pitch, uint32_t width, uint32_t height, uint32_t bpp);
int  fbcon_count(void);
void fbcon_clear(uint32_t argb);
void fbcon_setpos(uint32_t col, uint32_t row);
void fbcon_putc(char c);
void fbcon_puts(const char *s);
void fbcon_hex(uint64_t v);
void fbcon_dec(uint64_t v);

/**
 * Platform Expert (pexpert) implementation for Redmi 12 ("fire")
 * Target SoC: MediaTek Helio G88 (MT6769H)
 * Replaces Apple AIC and Apple proprietary board setup with MediaTek MMIO.
 */

#include "../include/platform/fire_mt6769.h"
#include <stdint.h>

/* Volatile MMIO pointer helper */
#define MMIO_READ32(addr)       (*(volatile uint32_t *)(addr))
#define MMIO_WRITE32(addr, val) (*(volatile uint32_t *)(addr) = (uint32_t)(val))

static uintptr_t g_uart_base = MT6769_UART0_BASE;

/**
 * Early UART Output for Kernel Printk / Panic
 */
void pe_fire_uart_putc(char c) {
    /* Wait until Transmitter Holding Register Empty (THRE) is set */
    while (!(MMIO_READ32(g_uart_base + UART_LSR) & UART_LSR_THRE)) {
        /* Busy wait */
    }
    MMIO_WRITE32(g_uart_base + UART_THR, (uint32_t)c);
}

void pe_fire_uart_puts(const char *str) {
    while (*str) {
        if (*str == '\n') {
            pe_fire_uart_putc('\r');
        }
        pe_fire_uart_putc(*str++);
    }
}

/**
 * Initialize ARM Generic Interrupt Controller (GIC) on MT6769
 */
void pe_fire_init_gic(void) {
    pe_fire_uart_puts("[pexpert/fire] Initializing MediaTek GIC Distributor...\n");
    
    /* Enable GIC Distributor */
    MMIO_WRITE32(MT6769_GICD_BASE + 0x000, 0x01); // GICD_CTLR: Enable group 0
    
    /* Enable GIC CPU Interface */
    MMIO_WRITE32(MT6769_GICC_BASE + 0x000, 0x01); // GICC_CTLR: Enable signaling
    MMIO_WRITE32(MT6769_GICC_BASE + 0x004, 0xF0); // GICC_PMR: Priority Mask
    
    pe_fire_uart_puts("[pexpert/fire] GICv2 mapped successfully for Helio G88.\n");
}

/**
 * Hardware board entry called by XNU start_first_cpu()
 */
int pe_fire_identify_machine(void) {
    pe_fire_uart_puts("\n==================================================\n");
    pe_fire_uart_puts("  DarwinFire: XNU Platform Expert on Redmi 12\n");
    pe_fire_uart_puts("  SoC: MediaTek Helio G88 (MT6769H) AArch64\n");
    pe_fire_uart_puts("==================================================\n");

    pe_fire_init_gic();
    return 0; // Success
}

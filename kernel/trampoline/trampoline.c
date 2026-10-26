/**
 * ARM64 Boot Trampoline for MediaTek Helio G88 (MT6769H / Redmi 12)
 * Bridges Android Fastboot / LittleKernel execution to Darwin XNU
 */

#include "../include/platform/fire_mt6769.h"
#include <stdint.h>

extern void pe_fire_uart_puts(const char *str);
extern int pe_fire_identify_machine(void);

/**
 * Linux ARM64 Boot State expectations:
 * - x0: Physical address of Flattened Device Tree (FDT) in RAM
 * - x1: 0 (reserved)
 * - x2: 0 (reserved)
 * - x3: 0 (reserved)
 * - CPU in EL1 or EL2, MMU disabled, Data Caches clean/invalidated
 */
void darwin_trampoline_c_entry(uint64_t fdt_addr) {
    pe_fire_uart_puts("\n[TRAMPOLINE] Reached Darwin Trampoline Entry via Fastboot!\n");
    pe_fire_uart_puts("[TRAMPOLINE] Received FDT Device Tree Address.\n");
    
    /* Call Platform Expert machine setup */
    pe_fire_identify_machine();

    pe_fire_uart_puts("[TRAMPOLINE] Memory & GIC mapped. Ready to branch to XNU Mach kernel entry.\n");
}

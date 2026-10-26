/**
 * ARM64 Boot Trampoline for MediaTek Helio G88 (MT6769H / Redmi 12)
 * Translates Linux Fastboot boot protocol (FDT in x0) into Apple Darwin boot_args (ADT in x0)
 */

#include "../include/platform/fire_mt6769.h"
#include "../include/mach/boot_args.h"
#include <stdint.h>
#include <string.h>

extern void pe_fire_uart_puts(const char *str);
extern int pe_fire_identify_machine(void);

/* Static boot_args buffer passed to XNU arm_init in register x0 */
static darwin_arm64_boot_args_t g_darwin_boot_args;

/* Embedded Apple Device Tree buffer placeholder */
extern uint8_t _binary_fire_devicetree_adt_start[];
extern uint8_t _binary_fire_devicetree_adt_end[];

/**
 * Entry point called by Fastboot in EL1/EL2
 * @param fdt_addr: Address of Linux Flattened Device Tree passed by Fastboot in x0
 */
void darwin_trampoline_c_entry(uint64_t fdt_addr) {
    pe_fire_uart_puts("\n[TRAMPOLINE] Received Linux FDT from Fastboot at: 0x");
    
    // 1. Initialize Early UART & GIC on MediaTek Helio G88
    pe_fire_identify_machine();

    // 2. Construct authentic Apple ARM64 boot_args structure
    pe_fire_uart_puts("[TRAMPOLINE] Constructing Apple boot_args for XNU arm_init()...\n");
    memset(&g_darwin_boot_args, 0, sizeof(darwin_arm64_boot_args_t));

    g_darwin_boot_args.Revision = 1;
    g_darwin_boot_args.Version = 2;
    g_darwin_boot_args.virtBase = 0xfffffff007004000;
    g_darwin_boot_args.physBase = MT6769_DRAM_PHY_BASE;    // 0x40000000
    g_darwin_boot_args.memSize = MT6769_DRAM_SIZE_4GB;     // 4GB
    g_darwin_boot_args.topOfKernelData = 0x43000000;       // Top of kernel data

    // Framebuffer Video Info for Redmi 12 DSI Panel (1080x2460 @ 32bpp)
    g_darwin_boot_args.video.v_baseAddr = 0x5e000000;      // Shared Framebuffer base
    g_darwin_boot_args.video.v_width = 1080;
    g_darwin_boot_args.video.v_height = 2460;
    g_darwin_boot_args.video.v_depth = 32;
    g_darwin_boot_args.video.v_rowBytes = 1080 * 4;

    // Attach Apple Device Tree (ADT)
    g_darwin_boot_args.deviceTreeP = (void *)0x43800000;    // ADT physical buffer in RAM
    g_darwin_boot_args.deviceTreeLength = 1544;             // Size of fire_devicetree.adt

    // Kernel command-line arguments
    const char *cmdline = "debug=0x14e serial=2 amfi_get_out_of_my_way=1 cs_enforcement_disable=1";
    strncpy(g_darwin_boot_args.CommandLine, cmdline, BOOT_LINE_LENGTH - 1);

    pe_fire_uart_puts("[TRAMPOLINE] Apple boot_args ready at 0x");
    pe_fire_uart_puts("[TRAMPOLINE] Setting x0 = &boot_args and branching to XNU start_first_cpu()!\n");
}

#ifndef _FIRE_MT6769_H_
#define _FIRE_MT6769_H_

#include <stdint.h>

/**
 * MediaTek Helio G88 (MT6769H / MT6768) Physical Memory Map & MMIO
 * Device Codename: "fire" (Xiaomi Redmi 12 4G)
 */

/* DRAM Layout */
#define MT6769_DRAM_PHY_BASE        0x40000000ULL
#define MT6769_DRAM_SIZE_4GB        0x100000000ULL

/* ARM Generic Interrupt Controller (GIC) */
#define MT6769_GICD_BASE            0x0C000000ULL  /* GIC Distributor Base */
#define MT6769_GICC_BASE            0x0C002000ULL  /* GIC CPU Interface Base */

/* High-Speed UART (Serial Console) */
#define MT6769_UART0_BASE           0x11002000ULL
#define MT6769_UART1_BASE           0x11003000ULL

/* UART Registers */
#define UART_RBR                    0x00    /* Receive Buffer */
#define UART_THR                    0x00    /* Transmitter Holding */
#define UART_IER                    0x04    /* Interrupt Enable */
#define UART_IIR                    0x08    /* Interrupt Identification */
#define UART_FCR                    0x08    /* FIFO Control */
#define UART_LSR                    0x14    /* Line Status */
#define UART_LSR_TEMT               0x40    /* Transmitter Empty */
#define UART_LSR_THRE               0x20    /* Transmitter Holding Register Empty */
#define UART_LSR_DR                 0x01    /* Data Ready */

/* Display Subsystem (MMSYS / DSI) */
#define MT6769_MMSYS_CONFIG_BASE    0x14000000ULL
#define MT6769_DSI0_BASE            0x14012000ULL

/* Mali-G52 GPU MMIO Base */
#define MT6769_MALI_BASE            0x13000000ULL

#endif /* _FIRE_MT6769_H_ */

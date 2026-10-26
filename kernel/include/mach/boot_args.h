#ifndef _ARM64_BOOT_ARGS_H_
#define _ARM64_BOOT_ARGS_H_

#include <stdint.h>

#define BOOT_LINE_LENGTH 256

typedef struct {
    uint32_t v_baseAddr;
    uint32_t v_display;
    uint32_t v_rowBytes;
    uint32_t v_width;
    uint32_t v_height;
    uint32_t v_depth;
} boot_video_t;

/**
 * Apple ARM64 boot_args structure expected by XNU arm_init()
 */
typedef struct {
    uint16_t        Revision;           /* Expected: 1 */
    uint16_t        Version;            /* Expected: 1 or 2 */
    uint32_t        virtBase;           /* 0xfffffff007004000 */
    uint32_t        physBase;           /* 0x40000000 (MediaTek DRAM base) */
    uint32_t        memSize;            /* 0x100000000 (4GB) */
    uint32_t        topOfKernelData;    /* End of kernel in physical memory */
    boot_video_t    video;              /* 1080x2460 DSI display info */
    uint32_t        machineType;        /* Board ID */
    void            *deviceTreeP;       /* Pointer to Apple Device Tree (ADT) */
    uint32_t        deviceTreeLength;   /* Size of ADT in bytes */
    char            CommandLine[BOOT_LINE_LENGTH]; /* Kernel arguments */
} __attribute__((packed)) darwin_arm64_boot_args_t;

#endif /* _ARM64_BOOT_ARGS_H_ */

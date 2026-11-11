#ifndef _ARM64_BOOT_ARGS_H_
#define _ARM64_BOOT_ARGS_H_

#include <stdint.h>

/* Layout of struct boot_args from open-source XNU pexpert/pexpert/arm64/boot.h (natural alignment, 64-bit).
 * shortcut: BOOT_LINE_LENGTH 608 matches the published arm64 header; if XNU reads garbage bootFlags, re-check it. */
#define BOOT_LINE_LENGTH   608
#define kBootArgsRevision2 2
#define kBootArgsVersion2  2

typedef struct {
    uint64_t v_baseAddr;
    uint64_t v_display;
    uint64_t v_rowBytes;
    uint64_t v_width;
    uint64_t v_height;
    uint64_t v_depth;
} boot_video_t;

typedef struct {
    uint16_t     Revision;
    uint16_t     Version;
    uint64_t     virtBase;
    uint64_t     physBase;
    uint64_t     memSize;
    uint64_t     topOfKernelData;
    boot_video_t video;
    uint32_t     machineType;
    uint64_t     deviceTreeP;        /* physical address of the device tree */
    uint32_t     deviceTreeLength;
    char         CommandLine[BOOT_LINE_LENGTH];
    uint64_t     bootFlags;
    uint64_t     memSizeActual;
} darwin_arm64_boot_args_t;

_Static_assert(__builtin_offsetof(darwin_arm64_boot_args_t, virtBase) == 8, "boot_args layout");
_Static_assert(__builtin_offsetof(darwin_arm64_boot_args_t, video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(darwin_arm64_boot_args_t, deviceTreeP) == 96, "boot_args layout");
_Static_assert(__builtin_offsetof(darwin_arm64_boot_args_t, CommandLine) == 108, "boot_args layout");

#endif /* _ARM64_BOOT_ARGS_H_ */

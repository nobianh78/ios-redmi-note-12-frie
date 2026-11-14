/*
 * fire bring-up: prefix header for the open-source HFS+ core (apple-oss-distributions/hfs,
 * hfs-627.100.6, the macOS 13.5 release) compiled into the kernel instead of HFS.kext.
 * Replaces core/kext-config.h (macOS branch). Options the kernel config already decides
 * (FIFO, NAMEDSTREAMS, CONFIG_PROTECT, QUOTA) are left to it, so HFS and the VFS agree.
 */
#ifndef DF_HFS_CONFIG_H
#define DF_HFS_CONFIG_H
#ifndef HFS_COMPRESSION
#define HFS_COMPRESSION 1
#endif
#ifndef CONFIG_HFS_DIRLINK
#define CONFIG_HFS_DIRLINK 1
#endif
#ifndef CONFIG_SEARCHFS
#define CONFIG_SEARCHFS 1
#endif

/* KPI functions that <sys/sysctl.h> etc. only declare for kexts (#ifndef XNU_KERNEL_PRIVATE). */
#include <sys/types.h>
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif

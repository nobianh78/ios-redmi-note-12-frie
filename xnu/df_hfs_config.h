/*
 * fire bring-up: prefix header for the open-source HFS+ core (apple-oss-distributions/hfs,
 * hfs-627.100.6, the macOS 13.5 release) compiled into the kernel instead of HFS.kext.
 *
 * HFS is written against the kext view of the kernel headers, but every file under bsd/ is
 * compiled with XNU_KERNEL_PRIVATE/BSD_KERNEL_PRIVATE and the kernel's config options on the
 * command line (first build: ubc_create_upl undeclared, half of hfs_cprotect.c visible). So give
 * these files exactly what a kext build sees: no XNU/BSD-internal view, no kernel config options,
 * and the options of core/kext-config.h (macOS branch). QUOTA stays off: the VMAPPLE kernel has
 * no quota support to call into.
 */
#ifndef DF_HFS_CONFIG_H
#define DF_HFS_CONFIG_H

#undef XNU_KERNEL_PRIVATE
#undef BSD_KERNEL_PRIVATE
#undef MACH_KERNEL_PRIVATE
#undef CONFIG_PROTECT
#undef CONFIG_MACF
#undef CONFIG_SECLUDED_RENAME
#undef QUOTA
#undef NAMEDSTREAMS
#undef FIFO
#undef CONFIG_PV_TICKET         /* locks.h: __startup_func lck_init_pv() exists only inside xnu */
#ifndef __startup_func
#define __startup_func          /* kern/startup.h is XNU_KERNEL_PRIVATE-only */
#endif

#define HFS_COMPRESSION 1
#define FIFO 1
#define QUOTA 0
#define NAMEDSTREAMS 1
#define CONFIG_HFS_DIRLINK 1
#define CONFIG_SEARCHFS 1
#define CONFIG_PROTECT 0

/* kext KPIs that the source-tree headers declare #ifndef XNU_KERNEL_PRIVATE are visible now.
 * sysctlbyname is exported to kexts but <sys/sysctl.h> only declares it for !KERNEL. */
#include <sys/types.h>
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif

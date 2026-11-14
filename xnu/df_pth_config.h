/*
 * fire bring-up: prefix header for libpthread's kernel part (apple-oss-distributions/libpthread,
 * libpthread-514.100.2, the macOS 13.5 release), compiled into the kernel instead of pthread.kext.
 * Same idea as df_hfs_config.h: give these files the kext view of the kernel headers.
 */
#ifndef DF_PTH_CONFIG_H
#define DF_PTH_CONFIG_H
#include "df_targetconditionals.h"
#undef XNU_KERNEL_PRIVATE
#undef BSD_KERNEL_PRIVATE
#undef MACH_KERNEL_PRIVATE
#undef CONFIG_PV_TICKET
#ifndef __startup_func
#define __startup_func
#endif
#endif

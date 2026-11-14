/*
 * fire bring-up: minimal <TargetConditionals.h> for the in-kernel HFS build. Kexts get this header
 * from the SDK; the xnu source tree has none, and kernel headers include it on the kext path
 * (#ifndef XNU_KERNEL_PRIVATE, e.g. osfmk/kern/debug.h). Values are those of a macOS arm64 kext,
 * matching core/kext-config.h's macOS branch. Installed as EXTERNAL_HEADERS/TargetConditionals.h.
 */
#ifndef __TARGETCONDITIONALS__
#define __TARGETCONDITIONALS__
#define TARGET_OS_MAC               1
#define TARGET_OS_OSX               1
#define TARGET_OS_IPHONE            0
#define TARGET_OS_IOS               0
#define TARGET_OS_MACCATALYST       0
#define TARGET_OS_TV                0
#define TARGET_OS_WATCH             0
#define TARGET_OS_BRIDGE            0
#define TARGET_OS_XR                0
#define TARGET_OS_VISION            0
#define TARGET_OS_DRIVERKIT         0
#define TARGET_OS_SIMULATOR         0
#define TARGET_OS_EMBEDDED          0
#define TARGET_OS_NANO              0
#define TARGET_IPHONE_SIMULATOR     0
#define TARGET_OS_UNIX              0
#define TARGET_OS_WIN32             0
#define TARGET_OS_WINDOWS           0
#define TARGET_OS_LINUX             0
#define TARGET_CPU_ARM64            1
#define TARGET_CPU_ARM              0
#define TARGET_CPU_X86_64           0
#define TARGET_CPU_X86              0
#define TARGET_CPU_PPC              0
#define TARGET_CPU_PPC64            0
#define TARGET_RT_64_BIT            1
#define TARGET_RT_LITTLE_ENDIAN     1
#define TARGET_RT_BIG_ENDIAN        0
#define TARGET_RT_MAC_MACHO         1
#endif

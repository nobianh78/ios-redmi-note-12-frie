/*
 * fire bring-up: in-kernel replacement for HFS.kext's IOKit glue (core/hfs_iokit.cpp).
 * There is no kext loading on fire, so the HFS+ core is compiled into the kernel and
 * df_hfs_start() does what com_apple_filesystems_hfs::start did: register the filesystem
 * with the VFS (before bsd_init mounts root) and set up HFS's zones and sysctls.
 * The media/platform/AKS helpers are stubbed: no IOMedia, no serial number, no keybag.
 */
#include "df_hfs_config.h"
#include <mach/mach_types.h>
#include <sys/mount.h>
#include <libkern/libkern.h>

#include "hfs_iokit.h"
#include "hfs.h"
#include "hfs_dbg.h"
#include "hfs_cnode.h"

#ifndef panic_on_assert
bool panic_on_assert;
#endif

extern void hfs_converterinit(void);

extern struct vnodeopv_desc hfs_vnodeop_opv_desc;
extern struct vnodeopv_desc hfs_specop_opv_desc;
#if FIFO
extern struct vnodeopv_desc hfs_fifoop_opv_desc;
#endif
extern struct vfsops hfs_vfsops;

static vfstable_t df_hfs_vfs_handle;

#ifndef VFS_TBLVNOP_SECLUDE_RENAME
#define VFS_TBLVNOP_SECLUDE_RENAME 0
#endif

void df_hfs_start(void);
void
df_hfs_start(void)
{
	struct vnodeopv_desc *op_descs[] = {
		&hfs_vnodeop_opv_desc,
		&hfs_specop_opv_desc,
#if FIFO
		&hfs_fifoop_opv_desc,
#endif
	};
	struct vfs_fsentry vfe = {
		.vfe_vfsops = &hfs_vfsops,
		.vfe_vopcnt = sizeof(op_descs) / sizeof(op_descs[0]),
		.vfe_opvdescs = op_descs,
		.vfe_fsname = "hfs",
		.vfe_flags = (VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBLREADDIR_EXTENDED
		    | VFS_TBL64BITREADY | VFS_TBLVNOP_PAGEOUTV2 | VFS_TBLVNOP_PAGEINV2
		    | VFS_TBLTHREADSAFE | VFS_TBLCANMOUNTROOT | VFS_TBLVNOP_SECLUDE_RENAME
		    | VFS_TBLNATIVEXATTR)
	};

	hfs_converterinit();            /* HFSEncodings.kext's start (hfs_encodings/hfs_encodings_kext.cpp) */
	int ret = vfs_fsadd(&vfe, &df_hfs_vfs_handle);
	if (ret) {
		printf("fire hfs: vfs_fsadd failed: %d\n", ret);
		df_hfs_vfs_handle = NULL;
		return;
	}
	hfs_init_zones();
	hfs_sysctl_register();
	printf("fire hfs: HFS+ registered (hfs-627.100.6, built into the kernel)\n");
}

int
hfs_is_ejectable(const char *cdev_name)
{
	(void)cdev_name;
	return 0;
}

void
hfs_iterate_media_with_content(const char *content_uuid_cstring,
    int (*func)(const char *device, const char *uuid_str, void *arg), void *arg)
{
	(void)content_uuid_cstring;
	(void)func;
	(void)arg;
}

kern_return_t
hfs_get_platform_serial_number(char *serial_number_str, uint32_t len)
{
	if (len > 0) {
		serial_number_str[0] = '\0';
	}
	return KERN_NOT_SUPPORTED;
}

#if CONFIG_PROTECT      /* off in the kext view (df_hfs_config.h); kept for an iOS-style config */
/* No AppleKeyStore on fire: content-protected volumes cannot be unlocked. */
int
hfs_unwrap_key(aks_cred_t access, const aks_wrapped_key_t wrapped_key_in, aks_raw_key_t key_out)
{
	(void)access; (void)wrapped_key_in; (void)key_out;
	return ENOTSUP;
}
int
hfs_rewrap_key(aks_cred_t access, cp_key_class_t dp_class, const aks_wrapped_key_t wrapped_key_in,
    aks_wrapped_key_t wrapped_key_out)
{
	(void)access; (void)dp_class; (void)wrapped_key_in; (void)wrapped_key_out;
	return ENOTSUP;
}
int
hfs_new_key(aks_cred_t access, cp_key_class_t dp_class, aks_raw_key_t key_out,
    aks_wrapped_key_t wrapped_key_out)
{
	(void)access; (void)dp_class; (void)key_out; (void)wrapped_key_out;
	return ENOTSUP;
}
int
hfs_backup_key(aks_cred_t access, const aks_wrapped_key_t wrapped_key_in,
    aks_wrapped_key_t wrapped_key_out)
{
	(void)access; (void)wrapped_key_in; (void)wrapped_key_out;
	return ENOTSUP;
}
#endif

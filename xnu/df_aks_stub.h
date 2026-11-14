/*
 * fire bring-up: stand-in for <AppleKeyStore/AppleKeyStoreFSServices.h> (a private header that is not
 * open source), declaring only what the HFS core uses. The kernel config keeps CONFIG_PROTECT on, so
 * hfs_cprotect.c builds against these; the AKS entry points in df_hfs_glue.c all return ENOTSUP
 * (no keybag on fire), so content-protected volumes cannot be unlocked.
 */
#ifndef DF_AKS_STUB_H
#define DF_AKS_STUB_H
#include <sys/types.h>
#include <sys/cprotect.h>
#include <uuid/uuid.h>

#define AKS_RAW_KEY_WRAPPEDKEY 0x1

struct aks_cred_s {
	pid_t pid;
	uid_t uid;
	uint64_t inode;
	uint32_t key_revision;
	uuid_t volume_uuid;
};
struct aks_raw_key_s {
	uint8_t *key;
	int key_len;
	uint8_t *iv_key;
	int iv_key_len;
	uint32_t flags;
};
struct aks_wrapped_key_s {
	uint8_t *key;
	int key_len;
	cp_key_class_t dp_class;
};
typedef struct aks_cred_s *aks_cred_t;
typedef struct aks_raw_key_s *aks_raw_key_t;
typedef struct aks_wrapped_key_s *aks_wrapped_key_t;
#endif

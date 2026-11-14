
/*
 * fire bring-up P5: /dev/emmc, a READ-ONLY character device over the whole eMMC user area
 * (df_emmc_read in df_emmc.c). Appended to bsd/dev/memdev.c by fix-xcode16.sh (it has the
 * cdevsw/devfs/uio headers). Byte offset = disk offset, so `xd /dev/emmc OFFSET LEN` reads any
 * partition at its GPT LBA * 512. Opening for writing fails with EROFS and d_write is eno_rdwrt:
 * there is no path from userland to an eMMC write.
 */
#include <kern/locks.h>

extern int df_emmc_read(uint32_t lba, void *buf);
extern uint64_t df_emmc_sectors;

static LCK_GRP_DECLARE(dfe_lck_grp, "df_emmc");
static LCK_MTX_DECLARE(dfe_mtx, &dfe_lck_grp);  /* one PIO command at a time */
static uint8_t dfe_dev_blk[512];

static int
dfe_dev_open(dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	(void)dev;
	return (flags & FWRITE) ? EROFS : 0;
}

static int
dfe_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
dfe_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int err = 0;
	lck_mtx_lock(&dfe_mtx);
	while (!err && uio_resid(uio) > 0) {
		off_t off = uio_offset(uio);
		uint64_t lba = (uint64_t)off >> 9;
		if (off < 0 || lba >= df_emmc_sectors) {
			break;                          /* EOF */
		}
		if (df_emmc_read((uint32_t)lba, dfe_dev_blk)) {
			err = EIO;
			break;
		}
		int in = (int)(off & 511), n = 512 - in;
		if (n > uio_resid(uio)) {
			n = (int)uio_resid(uio);
		}
		err = uiomove((caddr_t)dfe_dev_blk + in, n, uio);
	}
	lck_mtx_unlock(&dfe_mtx);
	return err;
}

static const struct cdevsw dfe_cdevsw = {
	.d_open       = dfe_dev_open,
	.d_close      = dfe_dev_close,
	.d_read       = dfe_dev_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = eno_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = 0,
};

void df_emmc_devinit(void);
void
df_emmc_devinit(void)
{
	if (!df_emmc_sectors) {
		printf("emmc: no disk, /dev/emmc not created\n");
		return;
	}
	int major = cdevsw_add(-1, &dfe_cdevsw);
	if (major < 0 || !devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0440, "emmc")) {
		printf("emmc: /dev/emmc failed (major %d)\n", major);
		return;
	}
	printf("emmc: /dev/emmc ready (read-only, %llu sectors)\n", df_emmc_sectors);
}

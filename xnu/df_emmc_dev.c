
/*
 * fire bring-up P5: /dev/emmc, a READ-ONLY character device over the whole eMMC user area
 * (df_emmc_read in df_emmc.c). Appended to bsd/dev/memdev.c by fix-xcode16.sh (it has the
 * cdevsw/devfs/uio headers). Byte offset = disk offset, so `xd /dev/emmc OFFSET LEN` reads any
 * partition at its GPT LBA * 512. Opening for writing fails with EROFS and d_write is eno_rdwrt:
 * there is no path from userland to an eMMC write.
 */
#include <kern/locks.h>

extern int df_emmc_readn(uint32_t lba, uint32_t n, void *buf);
extern uint64_t df_emmc_sectors;

static LCK_GRP_DECLARE(dfe_lck_grp, "df_emmc");
static LCK_MTX_DECLARE(dfe_mtx, &dfe_lck_grp);  /* one controller command at a time */
static uint8_t dfe_dev_buf[64 * 1024];

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
		/* the blocks covering this request, up to 64 KB, clipped at the end of the disk */
		int in = (int)(off & 511);
		uint64_t want = (uint64_t)in + (uint64_t)uio_resid(uio);
		uint32_t blks = (uint32_t)((want + 511) >> 9);
		if (blks > sizeof(dfe_dev_buf) / 512) {
			blks = sizeof(dfe_dev_buf) / 512;
		}
		if (lba + blks > df_emmc_sectors) {
			blks = (uint32_t)(df_emmc_sectors - lba);
		}
		int rc = df_emmc_readn((uint32_t)lba, blks, dfe_dev_buf);
		if (rc) {
			printf("emmc: read LBA %llu (+%u) failed %d\n", lba, blks, rc);
			err = EIO;
			break;
		}
		int n = (int)blks * 512 - in;
		if (n > uio_resid(uio)) {
			n = (int)uio_resid(uio);
		}
		err = uiomove((caddr_t)dfe_dev_buf + in, n, uio);
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

/* ---- P6: READ-ONLY block device over the GPT "super" partition, the HFS+ root ----
 * Device block = 512 bytes, block 0 = first LBA of super. Opening for write fails (EROFS) and the
 * strategy routine fails every non-read buffer with EROFS, so the root stays read-only: a remount
 * read-write is refused here instead of reaching the card. */
extern uint64_t df_emmc_super_lba, df_emmc_super_blocks;
extern void df_emmc_probe(void);
extern int df_emmc_read(uint32_t lba, void *buf);
static int dfe_bmajor = -1;

static int
dfe_bdev_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	return (flags & FWRITE) ? EROFS : 0;
}

/* fire diag: dyld reads happen at exec, long before the fault scrolls them off; keep them for the fault trace */
static uint32_t dfe_dyld_off[32], dfe_dyld_len[32];
static int dfe_dyld_n;
void df_emmc_dyld_dump(void);
void
df_emmc_dyld_dump(void)
{
	for (int i = 0; i < dfe_dyld_n; i++) {
		printf("emmc dyld rd#%d off=0x%x len=0x%x\n", i, dfe_dyld_off[i], dfe_dyld_len[i]);
	}
}

/* fire diag: set by sleh on the garbage-pointer fault; at PID 1 exit (thread context, copyin can page in)
 * print a 64-bit word sum of each 16K page of dyld's image (0..0x90000) to compare with the file. */
uint64_t df_dyld_base;
static uint64_t dfe_sum_buf[0x4000 / 8];
void df_dyld_sums(void);
void
df_dyld_sums(void)
{
	if (!df_dyld_base) {
		return;
	}
	(void)dfe_sum_buf; /* page sums retired: the garbage was unwritten zero blocks on super (img2simg DONT_CARE) */
}

/* fire diag: last path/shared-region syscalls of PID 1, printed at its exit (the live trace scrolls away) */
#define DFE_SC_N 24
static struct { int code, err; uint64_t rv; char path[80]; } dfe_sc[DFE_SC_N];
static int dfe_sc_i;
void df_sc_note(int code, uint64_t arg0, int err, uint64_t rv);
void
df_sc_note(int code, uint64_t arg0, int err, uint64_t rv)
{
	static char p[256];
	size_t l = 0;
	p[0] = 0;
	if (code == 5 || code == 338 || code == 340) {     /* open, stat64, lstat64: keep only shared-cache paths */
		if (copyinstr((user_addr_t)arg0, p, sizeof(p) - 1, &l)) {
			strlcpy(p, "<copyinstr failed>", sizeof(p));
		} else if (!strstr(p, "dyld") && !strstr(p, "Cryptex") && !strstr(p, "Caches")) {
			return;
		}
	}
	int k = dfe_sc_i++ % DFE_SC_N;
	size_t n = strlen(p);
	dfe_sc[k].code = code;
	dfe_sc[k].err = err;
	dfe_sc[k].rv = rv;
	strlcpy(dfe_sc[k].path, n > 56 ? p + n - 56 : p, sizeof(dfe_sc[k].path));   /* the tail names the file */
}
void df_sc_dump(void);
void
df_sc_dump(void)
{
	int n = dfe_sc_i < DFE_SC_N ? dfe_sc_i : DFE_SC_N;
	printf("sc total %d (cache-related)\n", dfe_sc_i);
	for (int j = 0; j < n; j++) {
		int k = (dfe_sc_i - n + j) % DFE_SC_N;
		printf("sc %d err %d rv 0x%llx %s\n", dfe_sc[k].code, dfe_sc[k].err, dfe_sc[k].rv, dfe_sc[k].path);
	}
}

static void
dfe_bdev_strategy(buf_t bp)
{
	uint64_t blk = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp);
	caddr_t va = 0;
	int err = 0;
	if (!(buf_flags(bp) & B_READ)) {
		err = EROFS;
	} else if ((count & 511) || blk + (count >> 9) > df_emmc_super_blocks) {
		err = EINVAL;
	} else if ((err = buf_map(bp, &va)) == 0) {
		/* fire diag: reads touching /usr/lib/dyld (HFS block 2094448 = sector 16755584, 1811 sectors) */
		if (blk < 16755584ull + 1811 && blk + (count >> 9) > 16755584ull && dfe_dyld_n < 32) {
			dfe_dyld_off[dfe_dyld_n] = (uint32_t)((blk - 16755584ull) << 9);
			dfe_dyld_len[dfe_dyld_n++] = count;
		}
		lck_mtx_lock(&dfe_mtx);
		if (df_emmc_readn((uint32_t)(df_emmc_super_lba + blk), count >> 9, va)) {
			err = EIO;
		}
		lck_mtx_unlock(&dfe_mtx);
		buf_unmap(bp);
	}
	if (err) {
		printf("emmc: super blk %llu len %u: error %d\n", blk, count, err);
		buf_seterror(bp, err);
		buf_setresid(bp, count);
	} else {
		buf_setresid(bp, 0);
	}
	buf_biodone(bp);
}

static uint32_t dfe_block_size = 512;

static int
dfe_bdev_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = dfe_block_size;
		return 0;
	case DKIOCSETBLOCKSIZE: {
		uint32_t sz = *(uint32_t *)data;
		if (sz < 512 || (sz & (sz - 1))) {
			return EINVAL;
		}
		dfe_block_size = sz;
		return 0;
	}
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = df_emmc_super_blocks * 512 / dfe_block_size;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETFEATURES:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCISSOLIDSTATE:
		*(uint32_t *)data = 1;
		return 0;
	case DKIOCISVIRTUAL:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCSYNCHRONIZECACHE:
		return 0;
	default:
		return ENOTTY;
	}
}

static int
dfe_bdev_psize(__unused dev_t dev)
{
	return (int)(df_emmc_super_blocks > 0x7fffffff ? 0x7fffffff : df_emmc_super_blocks);
}

static const struct bdevsw dfe_bdevsw = {
	.d_open     = dfe_bdev_open,
	.d_close    = dfe_dev_close,
	.d_strategy = dfe_bdev_strategy,
	.d_ioctl    = dfe_bdev_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = dfe_bdev_psize,
	.d_type     = D_DISK,
};

/* Called from setconf, after IOFindBSDRoot: if "super" holds an HFS+/HFSX volume (signature "H+"/"HX"
 * at byte 1024), it becomes the root device; otherwise NODEV and the RAMDisk (rd=md0) stays root. */
dev_t df_emmc_rootdev(void);
dev_t
df_emmc_rootdev(void)
{
	df_emmc_probe();
	if (!df_emmc_super_blocks) {
		printf("emmc: no super partition, root stays on the RAMDisk\n");
		return NODEV;
	}
	lck_mtx_lock(&dfe_mtx);
	int rc = df_emmc_read((uint32_t)df_emmc_super_lba + 2, dfe_dev_buf);
	lck_mtx_unlock(&dfe_mtx);
	if (rc || !(dfe_dev_buf[0] == 'H' && (dfe_dev_buf[1] == '+' || dfe_dev_buf[1] == 'X'))) {
		printf("emmc: super has no HFS+ volume header (rc %d, %02x %02x), root stays on the RAMDisk\n", rc,
		    dfe_dev_buf[0], dfe_dev_buf[1]);
		return NODEV;
	}
	if (dfe_bmajor < 0 && (dfe_bmajor = bdevsw_add(-1, &dfe_bdevsw)) < 0) {
		printf("emmc: bdevsw_add failed %d, root stays on the RAMDisk\n", dfe_bmajor);
		return NODEV;
	}
	printf("emmc: HFS+ on super (LBA %llu, %llu MB): root = block device %d,0 (read-only)\n",
	    df_emmc_super_lba, df_emmc_super_blocks >> 11, dfe_bmajor);
	return makedev(dfe_bmajor, 0);
}

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
	if (dfe_bmajor >= 0) {
		devfs_make_node(makedev(dfe_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0440, "emmc_super");
	}
}

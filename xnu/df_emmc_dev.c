
/*
 * fire bring-up P5: /dev/emmc, a READ-ONLY character device over the whole eMMC user area
 * (df_emmc_read in df_emmc.c). Appended to bsd/dev/memdev.c by fix-xcode16.sh (it has the
 * cdevsw/devfs/uio headers). Byte offset = disk offset, so `xd /dev/emmc OFFSET LEN` reads any
 * partition at its GPT LBA * 512. Opening for writing fails with EROFS and d_write is eno_rdwrt:
 * there is no path from userland to an eMMC write.
 */
#include <kern/locks.h>
#include <kern/kalloc.h>

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
dfe_bdev_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;   /* writable: writes go to the RAM overlay below, never to the card */
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
#define DFE_SC_N 64
static struct { int code, err; uint64_t rv; char path[112]; } dfe_sc[DFE_SC_N];
static int dfe_sc_i;
void df_sc_note(int code, uint64_t arg0, int err, uint64_t rv, uint64_t arg2, uint64_t arg3);
void
df_sc_note(int code, uint64_t arg0, int err, uint64_t rv, uint64_t arg2, uint64_t arg3)
{
	static char p[256];
	size_t l = 0;
	p[0] = 0;
	if (code == 360 || code == 366 || code == 367 || code == 368) {   /* bsdthread_create/register, workq_open/kernreturn */
		snprintf(p, sizeof(p), "op/a0 0x%llx a1 0x%llx", arg0, arg2);
	} else if (code == 92 || code == 153 || code == 197) {    /* fcntl(fd,cmd,arg) / pread(fd,buf,n,off) / mmap(addr,len,prot,flags,fd,off): arg2 = uu_arg[1], arg3 = uu_arg[2] */
		snprintf(p, sizeof(p), "a0 %llx a1 %llx a3 %llx", arg0, arg2, arg3);   /* arg3 carries uu_arg[3] */
	} else if (code == 220) {                                  /* getattrlist(path, alist, buf, size): show path + reply */
		char q[48] = {0};
		uint8_t r[24] = {0};
		if (copyinstr((user_addr_t)arg0, q, sizeof(q) - 1, &l)) {
			q[0] = 0;
		}
		/* dyld asks DEVID|FSID + VOL_CAPABILITIES|VOL_UUID: 64-byte reply, UUID at +0x30. Show 0x28..0x3f */
		int ce = err ? -1 : copyin((user_addr_t)arg2 + 0x28, r, sizeof(r));
		snprintf(p, sizeof(p), "%s ce%d %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
		    q, ce, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
		    r[12], r[13], r[14], r[15], r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23]);
	} else if (code == 347) {                           /* getfsstat64(buf, size, flags): f_fsid (+0x30) and f_mntonname (+0x58) per entry */
		int o = snprintf(p, sizeof(p), "n %llu", rv);
		for (uint64_t e = 0; arg0 && !err && e < rv && e < 2; e++) {
			uint32_t fsid[2] = {0, 0};
			char mn[16] = {0};
			int c1 = copyin((user_addr_t)(arg0 + e * 0x878 + 0x30), fsid, sizeof(fsid));
			int c2 = copyin((user_addr_t)(arg0 + e * 0x878 + 0x58), mn, sizeof(mn) - 1);
			o += snprintf(p + o, sizeof(p) - o, " [%s fsid %x:%x c%d%d]", mn, fsid[0], fsid[1], c1, c2);
		}
	} else if (code == 339) {                                  /* fstat64(fd, struct stat64 *): dev, mode, nlink, ino */
		struct { int32_t dev; uint16_t mode, nlink; uint64_t ino; } st = {0, 0, 0, 0};
		int ce = copyin((user_addr_t)arg2, &st, sizeof(st));   /* arg2 carries uu_arg[1] for 339, see systemcalls hook */
		snprintf(p, sizeof(p), "fd %llu dev %x mode %o nlink %u ino 0x%llx ce%d", arg0, st.dev, st.mode, st.nlink, st.ino, ce);
	} else if (code == 427) {                                  /* fsgetpath(buf, size, fsid_t *, objid) */
		uint32_t fsid[2] = {0, 0};
		int ce = copyin((user_addr_t)arg2, fsid, sizeof(fsid));
		snprintf(p, sizeof(p), "fsid %x:%x objid 0x%llx ce%d", fsid[0], fsid[1], arg3, ce);
	} else if (code == 244) {                           /* posix_spawn(pid*, path, ...): arg2 carries the path pointer */
		if (copyinstr((user_addr_t)arg2, p, sizeof(p) - 1, &l)) {
			strlcpy(p, "<spawn path?>", sizeof(p));
		}
	} else if (code == 463) {                           /* openat(fd, path, ...): arg2 carries the path pointer */
		if (copyinstr((user_addr_t)arg2, p, sizeof(p) - 1, &l)) {
			p[0] = 0;
		}
	} else if (code == 5 || code == 338 || code == 340) {     /* open, stat64, lstat64 */
		if (!arg0 && code == 338) {                      /* stat(NULL): arg2 = user lr, arg3 = user fp -> who called it */
			int o = snprintf(p, sizeof(p), "NULL lr %llx", arg2);
			uint64_t fp = arg3;
			for (int f = 0; f < 4 && fp; f++) {
				uint64_t fr[2] = {0, 0};
				if (copyin((user_addr_t)fp, fr, sizeof(fr))) {
					break;
				}
				o += snprintf(p + o, sizeof(p) - o, " %llx", fr[1]);
				fp = fr[0];
			}
		} else if (copyinstr((user_addr_t)arg0, p, sizeof(p) - 1, &l)) {
			snprintf(p, sizeof(p), "<copyinstr failed, ptr 0x%llx>", arg0);
		}
	}
	int k = dfe_sc_i++ % DFE_SC_N;
	size_t n = strlen(p);
	dfe_sc[k].code = code;
	dfe_sc[k].err = err;
	dfe_sc[k].rv = rv;
	strlcpy(dfe_sc[k].path, n > 110 ? p + n - 110 : p, sizeof(dfe_sc[k].path));   /* the tail names the file */
}
/* fire diag: kevent_qos(kq, changelist, nchanges, ...) of PID 1: what it registers before it blocks.
 * struct kevent_qos_s = ident u64, filter i16, flags u16, qos i32, udata u64, fflags u32, xflags u32,
 * data i64, ext[4] u64 = 72 bytes. EVFILT_TIMER is -7, EVFILT_MACHPORT -8, EVFILT_SIGNAL -6. */
void df_kev(uint64_t changelist, uint64_t nchanges, uint64_t flags);
void
df_kev(uint64_t changelist, uint64_t nchanges, uint64_t flags)
{
	static int n;
	if (n++ > 400) {
		return;
	}
	for (uint64_t i = 0; i < nchanges && i < 6; i++) {
		uint8_t k[72];
		if (copyin((user_addr_t)(changelist + i * 72), k, sizeof(k))) {
			break;
		}
		int16_t filter;
		uint16_t kflags;
		uint32_t fflags;
		int64_t data;
		uint64_t ident;
		memcpy(&ident, k + 0, 8);
		memcpy(&filter, k + 8, 2);
		memcpy(&kflags, k + 10, 2);
		memcpy(&fflags, k + 24, 4);
		memcpy(&data, k + 32, 8);
		printf("kev ch filt %d id 0x%llx fl 0x%x ffl 0x%x data %lld kfl 0x%llx\n", filter, ident, kflags, fflags, data, flags);
	}
}

/* fire diag: launchd blocks in kevent_qos (374); print what it opened/spawned since the last wait, once */
void df_sc_dump_new(void);
void
df_sc_dump_new(void)
{
	/* reprint the whole ring whenever something new happened: earlier dumps scroll off the screen */
	static int done;
	if (done == dfe_sc_i) {
		return;
	}
	int from = dfe_sc_i > DFE_SC_N ? dfe_sc_i - DFE_SC_N : 0;
	printf("sc ring %d..%d\n", from, dfe_sc_i);
	for (int j = from; j < dfe_sc_i; j++) {
		int k = j % DFE_SC_N;
		printf("sc %d err %d rv 0x%llx %s\n", dfe_sc[k].code, dfe_sc[k].err, dfe_sc[k].rv, dfe_sc[k].path);
	}
	done = dfe_sc_i;
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

/* Writes to the root never reach the card: they land in a RAM copy-on-write overlay of 4 KB pages
 * (lost at reboot). Open-addressing table page -> 4 KB buffer, filled from the eMMC on first write. */
#define DFE_OV_SLOTS (1u << 17)   /* up to 128K pages = 512 MB of changed data */
static uint64_t *dfe_ov_key;     /* page + 1, 0 = empty */
static uint8_t **dfe_ov_val;
static uint32_t dfe_ov_n;

static uint8_t *
dfe_ov_find(uint64_t page, int create)
{
	if (!dfe_ov_key) {
		if (!create) {
			return NULL;
		}
		dfe_ov_key = kalloc_data(DFE_OV_SLOTS * sizeof(uint64_t), Z_WAITOK | Z_ZERO);
		dfe_ov_val = kalloc_data(DFE_OV_SLOTS * sizeof(uint8_t *), Z_WAITOK | Z_ZERO);
		if (!dfe_ov_key || !dfe_ov_val) {
			return NULL;
		}
	}
	uint32_t h = (uint32_t)((page * 0x9E3779B97F4A7C15ull) >> 47) & (DFE_OV_SLOTS - 1);
	for (uint32_t i = 0; i < DFE_OV_SLOTS; i++, h = (h + 1) & (DFE_OV_SLOTS - 1)) {
		if (dfe_ov_key[h] == page + 1) {
			return dfe_ov_val[h];
		}
		if (!dfe_ov_key[h]) {
			if (!create || dfe_ov_n >= DFE_OV_SLOTS * 3 / 4) {
				return NULL;
			}
			uint8_t *v = kalloc_data(4096, Z_WAITOK);
			if (!v || df_emmc_readn((uint32_t)(df_emmc_super_lba + page * 8), 8, v)) {
				return NULL;
			}
			dfe_ov_val[h] = v;
			dfe_ov_key[h] = page + 1;
			dfe_ov_n++;
			if ((dfe_ov_n & 1023) == 1) {
				printf("emmc: RAM overlay %u pages (%u MB)\n", dfe_ov_n, dfe_ov_n / 256);
			}
			return v;
		}
	}
	return NULL;
}

/* sector-granular I/O through the overlay: blk/count in 512-byte sectors */
static int
dfe_ov_io(uint64_t blk, uint32_t nsec, uint8_t *va, int write)
{
	while (nsec) {
		uint64_t page = blk >> 3;
		uint32_t off = (uint32_t)(blk & 7), n = 8 - off;
		if (n > nsec) {
			n = nsec;
		}
		uint8_t *pg = dfe_ov_find(page, write);
		if (write) {
			if (!pg) {
				return ENOSPC;
			}
			memcpy(pg + off * 512, va, n * 512);
		} else if (pg) {
			memcpy(va, pg + off * 512, n * 512);
		} else if (df_emmc_readn((uint32_t)(df_emmc_super_lba + blk), n, va)) {
			return EIO;
		}
		blk += n;
		nsec -= n;
		va += n * 512;
	}
	return 0;
}

static void
dfe_bdev_strategy(buf_t bp)
{
	uint64_t blk = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp);
	caddr_t va = 0;
	int err = 0;
	if (!(buf_flags(bp) & B_READ)) {
		if ((count & 511) || blk + (count >> 9) > df_emmc_super_blocks) {
			err = EINVAL;
		} else if ((err = buf_map(bp, &va)) == 0) {
			static int dfe_wn;
			int dfe_w = 0 && dfe_wn++ < 200;
			if (dfe_w) { printf("emmc W> blk %llu n %u\n", blk, count >> 9); }
			lck_mtx_lock(&dfe_mtx);
			err = dfe_ov_io(blk, count >> 9, (uint8_t *)va, 1);
			lck_mtx_unlock(&dfe_mtx);
			if (dfe_w) { printf("emmc W< err %d ov %u\n", err, dfe_ov_n); }
			buf_unmap(bp);
		}
	} else if (dfe_ov_key) {   /* something was written: reads go through the overlay */
		if ((count & 511) || blk + (count >> 9) > df_emmc_super_blocks) {
			err = EINVAL;
		} else if ((err = buf_map(bp, &va)) == 0) {
			static int dfe_rn;
			int dfe_r = 0 && dfe_rn++ < 200;
			if (dfe_r) { printf("emmc R> blk %llu n %u\n", blk, count >> 9); }
			lck_mtx_lock(&dfe_mtx);
			err = dfe_ov_io(blk, count >> 9, (uint8_t *)va, 0);
			lck_mtx_unlock(&dfe_mtx);
			if (dfe_r) { printf("emmc R< err %d\n", err); }
			buf_unmap(bp);
		}
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
		*(uint32_t *)data = 1;
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

int df_tail_on;   /* set when launchd opens its lo0 socket: trace every pid 1 syscall from there on */

/* fire diag: every 15 s print where the threads of pids 1..40 sleep (uu_wmesg / uu_wchan): finds the hang */
#include <sys/proc_internal.h>
#include <sys/user.h>
#include <kern/thread_call.h>
static thread_call_t dfw_tc;
static void
dfw_tick(__unused thread_call_param_t a, __unused thread_call_param_t b)
{
	static int round;
	printf("fire watch %d\n", ++round);
	for (int pid = 1; pid <= 40; pid++) {
		proc_t p = proc_find(pid);
		if (p == PROC_NULL) {
			continue;
		}
		uthread_t uth;
		proc_lock(p);
		TAILQ_FOREACH(uth, &p->p_uthlist, uu_list) {
			if (uth->uu_wmesg || uth->uu_wchan) {
				printf("fire w pid %d %s t%x wmesg %s wchan %p\n", pid, proc_best_name(p),
				    (unsigned)(((uintptr_t)uth >> 4) & 0xffff), uth->uu_wmesg ? uth->uu_wmesg : "-", uth->uu_wchan);
			}
		}
		proc_unlock(p);
		proc_rele(p);
	}
	uint64_t dl;
	clock_interval_to_deadline(15, NSEC_PER_SEC, &dl);
	thread_call_enter_delayed(dfw_tc, dl);
}
void df_watch_start(void);
void
df_watch_start(void)
{
	if (dfw_tc) {
		return;
	}
	dfw_tc = thread_call_allocate(dfw_tick, NULL);
	uint64_t dl;
	clock_interval_to_deadline(30, NSEC_PER_SEC, &dl);
	thread_call_enter_delayed(dfw_tc, dl);
}

/* fix-xcode16 P6: sample backboardd's user PC/LR on timer IRQs (ring of 64), dumped when ps exits. */
uint64_t df_bb_ring[64][2];
unsigned df_bb_n;

void
df_bb_sample(uint64_t pc, uint64_t lr)
{
	const char *n = proc_best_name(current_proc());
	if (n && strcmp(n, "backboardd") == 0) {
		unsigned i = df_bb_n++ & 63;
		df_bb_ring[i][0] = pc;
		df_bb_ring[i][1] = lr;
	}
}

void
df_bb_dump(int pid)
{
	static int last;
	if (pid == last) {
		return;
	}
	last = pid;
	unsigned n = df_bb_n;
	printf("BBPC samples %u\n", n);
	for (unsigned i = 0; i < 64 && i < n; i++) {
		unsigned k = (n - 1 - i) & 63;
		printf("BBPC pc 0x%llx lr 0x%llx\n", df_bb_ring[k][0], df_bb_ring[k][1]);
	}
}

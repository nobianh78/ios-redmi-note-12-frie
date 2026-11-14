
/*
 * fire bring-up P5: READ-ONLY eMMC access through MSDC0 (MT6768, 0x11230000), polled PIO.
 * Appended to osfmk/arm64/amcc_rorgn_common.c after df_usb.c by fix-xcode16.sh.
 *
 * LK already initialised the controller, clocks and card (it loaded boot_b from this eMMC), and leaves
 * the card in the transfer state in the user area, so this only switches the controller to PIO and
 * issues single-block reads. SAFETY: the only commands ever sent are CMD13 (status) and CMD17 (read
 * one block); there is no write path at all, so userdata and every other partition stay untouched.
 * Register map: Linux drivers/mmc/host/mtk-sd.c.
 * shortcut: one 512-byte block per command, no IRQ/DMA; fine for GPT and reading files, slow for bulk.
 */

#define DFE_BASE        0x11230000UL
#define E_CFG           0x00    /* bit3 PIO */
#define E_INT           0x0c    /* write 1 to clear */
#define E_INTEN         0x10
#define E_FIFOCS        0x14    /* rx count 7:0, clear bit31 */
#define E_RXDATA        0x1c
#define E_SDC_CMD       0x34
#define E_SDC_ARG       0x38
#define E_SDC_STS       0x3c    /* bit0 sdc busy, bit1 cmd busy */
#define E_SDC_RESP0     0x40
#define E_BLK_NUM       0x50

#define EI_CMDRDY       (1u << 8)
#define EI_CMDTMO       (1u << 9)
#define EI_RSPCRCERR    (1u << 10)
#define EI_XFER_COMPL   (1u << 12)
#define EI_DATTMO       (1u << 14)
#define EI_DATCRCERR    (1u << 15)

extern int printf(const char *fmt, ...);
static volatile uint8_t *dfe;
static inline uint32_t dfe_r(unsigned o) { return *(volatile uint32_t *)(dfe + o); }
static inline void dfe_w(unsigned o, uint32_t v) { *(volatile uint32_t *)(dfe + o) = v; }

/* spin up to ~ms milliseconds for any of `bits` in MSDC_INT; returns the INT value seen (0 = timeout) */
static uint32_t
dfe_wait(uint32_t bits, int ms)
{
	for (int i = 0; i < ms * 100; i++) {
		uint32_t v = dfe_r(E_INT);
		if (v & bits) {
			return v;
		}
		for (volatile int d = 0; d < 1000; d++) {
		}
	}
	return 0;
}

/* R1 response. Read-only by construction: bit13 (write) is never set by any caller. */
static int
dfe_cmd(unsigned op, uint32_t arg, uint32_t extra, uint32_t *resp)
{
	for (int i = 0; dfe_r(E_SDC_STS) & 3; i++) {
		if (i > 1000000) {
			return -1;
		}
	}
	dfe_w(E_INT, dfe_r(E_INT));
	dfe_w(E_SDC_ARG, arg);
	dfe_w(E_SDC_CMD, op | (1u << 7) | extra);       /* extra: data type, block size, auto-CMD12 */
	uint32_t v = dfe_wait(EI_CMDRDY | EI_CMDTMO | EI_RSPCRCERR, 100);
	dfe_w(E_INT, EI_CMDRDY | EI_CMDTMO | EI_RSPCRCERR);
	if (!(v & EI_CMDRDY)) {
		return v ? -2 : -3;
	}
	if (resp) {
		*resp = dfe_r(E_SDC_RESP0);
	}
	return 0;
}

int df_emmc_read(uint32_t lba, void *buf);
int
df_emmc_read(uint32_t lba, void *buf)
{
	uint32_t *p = (uint32_t *)buf, r1;
	dfe_w(E_CFG, dfe_r(E_CFG) | (1u << 3));             /* PIO (the DMA path clears it) */
	dfe_w(E_FIFOCS, dfe_r(E_FIFOCS) | (1u << 31));
	for (int i = 0; dfe_r(E_FIFOCS) & (1u << 31); i++) {
		if (i > 1000000) {
			return -1;
		}
	}
	dfe_w(E_BLK_NUM, 1);
	int rc = dfe_cmd(17, lba, (1u << 11) | (512u << 16), &r1);
	if (rc) {
		return rc;
	}
	for (int got = 0, spin = 0; got < 128;) {
		if ((dfe_r(E_FIFOCS) & 0xff) >= 4) {
			p[got++] = dfe_r(E_RXDATA);
			spin = 0;
		} else if (dfe_r(E_INT) & (EI_DATTMO | EI_DATCRCERR)) {
			return -4;
		} else if (++spin > 10000000) {
			return -5;
		}
	}
	uint32_t v = dfe_wait(EI_XFER_COMPL | EI_DATTMO | EI_DATCRCERR, 100);
	dfe_w(E_INT, v);
	return (v & EI_XFER_COMPL) ? 0 : -6;
}

static uint8_t dfe_blk[512];

/* ---- basic-mode DMA (one physically contiguous buffer, no descriptors): CMD18 + auto CMD12 ---- */
#define E_DMA_SA        0x90
#define E_DMA_CTRL      0x98    /* bit0 start, bit1 stop, bit8 descriptor mode, bit10 last buf, 14:12 burst */
#define E_DMA_CFG       0x9c    /* bit0 active */
#define E_DMA_LEN       0xa8
#define DFE_DMA_BLKS    128     /* 64 KB per command */
extern void flush_dcache(vm_offset_t addr, unsigned count, int phys);
extern uint64_t kvtophys(vm_offset_t va);
extern uint64_t mach_absolute_time(void);
extern void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
/* in the kernel image, which the bootloader loaded physically contiguous (checked in dfe_dma_init) */
static uint8_t dfe_dma_buf[DFE_DMA_BLKS * 512] __attribute__((aligned(4096)));
static uint32_t dfe_dma_pa;
static int dfe_dma_ok;

static int
dfe_dma_read(uint32_t lba, uint32_t n)
{
	uint32_t len = n * 512, r1;
	flush_dcache((vm_offset_t)dfe_dma_buf, len, 0);     /* no dirty line may land on top of DMA data */
	dfe_w(E_CFG, dfe_r(E_CFG) & ~(1u << 3));            /* DMA, not PIO */
	dfe_w(E_FIFOCS, dfe_r(E_FIFOCS) | (1u << 31));
	for (int i = 0; dfe_r(E_FIFOCS) & (1u << 31); i++) {
		if (i > 1000000) {
			return -1;
		}
	}
	dfe_w(E_DMA_SA, dfe_dma_pa);
	dfe_w(E_DMA_LEN, len);
	/* basic mode, 64 B bursts; clear start/stop: the stop bit stays set after a transfer and would block the next one */
	dfe_w(E_DMA_CTRL, (dfe_r(E_DMA_CTRL) & ~(3u | (1u << 8) | (7u << 12))) | (1u << 10) | (6u << 12));
	dfe_w(E_BLK_NUM, n);
	int rc = dfe_cmd(n > 1 ? 18 : 17, lba, (n > 1 ? (2u << 11) | (1u << 28) : (1u << 11)) | (512u << 16), &r1);
	if (rc == 0) {
		dfe_w(E_DMA_CTRL, dfe_r(E_DMA_CTRL) | 1);       /* start */
		uint32_t v = dfe_wait(EI_XFER_COMPL | EI_DATTMO | EI_DATCRCERR, 2000);
		rc = (v & EI_XFER_COMPL) ? 0 : (v ? -8 : -7);
		for (int i = 0; (dfe_r(E_DMA_CFG) & 1) && i < 1000000; i++) {
		}
	}
	dfe_w(E_DMA_CTRL, dfe_r(E_DMA_CTRL) | 2);           /* stop (no-op when already idle) */
	for (int i = 0; (dfe_r(E_DMA_CFG) & 1) && i < 1000000; i++) {
	}
	dfe_w(E_INT, dfe_r(E_INT));
	flush_dcache((vm_offset_t)dfe_dma_buf, len, 0);     /* drop lines fetched before the DMA finished */
	return rc;
}

/* Read n blocks into buf: DMA in 64 KB commands when the self-test passed, else PIO block by block. */
int df_emmc_readn(uint32_t lba, uint32_t n, void *buf);
int
df_emmc_readn(uint32_t lba, uint32_t n, void *buf)
{
	uint8_t *out = (uint8_t *)buf;
	while (n) {
		uint32_t k = n < DFE_DMA_BLKS ? n : DFE_DMA_BLKS;
		int rc;
		if (dfe_dma_ok) {
			if ((rc = dfe_dma_read(lba, k))) {
				return rc;
			}
			__builtin_memcpy(out, dfe_dma_buf, k * 512);
		} else {
			for (uint32_t i = 0; i < k; i++) {
				if ((rc = df_emmc_read(lba + i, out + i * 512))) {
					return rc;
				}
			}
		}
		lba += k;
		n -= k;
		out += k * 512;
	}
	return 0;
}

static uint64_t
dfe_ms_since(uint64_t t0)
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns / 1000000;
}

/* Enable DMA only if the buffer is contiguous below 4 GB and DMA data matches PIO data. */
static void
dfe_dma_init(uint32_t bench_lba)
{
	uint64_t pa = kvtophys((vm_offset_t)dfe_dma_buf);
	for (unsigned off = 4096; off < sizeof(dfe_dma_buf); off += 4096) {
		if (kvtophys((vm_offset_t)dfe_dma_buf + off) != pa + off) {
			printf("emmc: DMA buffer not contiguous, staying on PIO\n");
			return;
		}
	}
	if (!pa || pa + sizeof(dfe_dma_buf) > 0x100000000ULL) {
		printf("emmc: DMA buffer at %llx, not below 4 GB, staying on PIO\n", pa);
		return;
	}
	dfe_dma_pa = (uint32_t)pa;
	/* two transfers back to back, both checked against PIO: a DMA that only works once must fail here */
	int rc = 0;
	for (uint32_t base = 0; base < 128 && !rc; base += 64) {
		if ((rc = dfe_dma_read(base, 64))) {
			printf("emmc: DMA test read at %u failed %d (int %x, dma cfg %x), staying on PIO\n", base, rc,
			    dfe_r(E_INT), dfe_r(E_DMA_CFG));
			return;
		}
		for (uint32_t b = 0; b < 64; b++) {
			if (df_emmc_read(base + b, dfe_blk)) {
				printf("emmc: PIO compare read failed at %u\n", base + b);
				return;
			}
			for (int i = 0; i < 512; i++) {
				if (dfe_blk[i] != dfe_dma_buf[b * 512 + i]) {
					printf("emmc: DMA/PIO mismatch at LBA %u byte %d, staying on PIO\n", base + b, i);
					return;
				}
			}
		}
	}	/* speed: 16 MB by DMA vs 1 MB by PIO, both from bench_lba */
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < 256 && !rc; i++) {
		rc = dfe_dma_read(bench_lba + i * DFE_DMA_BLKS, DFE_DMA_BLKS);
	}
	uint64_t dma_ms = dfe_ms_since(t0);
	t0 = mach_absolute_time();
	for (uint32_t i = 0; i < 2048 && !rc; i++) {
		rc = df_emmc_read(bench_lba + i, dfe_blk);
	}
	uint64_t pio_ms = dfe_ms_since(t0);
	if (rc) {
		printf("emmc: DMA benchmark failed %d, staying on PIO\n", rc);
		return;
	}
	dfe_dma_ok = 1;
	printf("emmc: DMA on (buffer pa %x): 16 MB in %llu ms = %llu KB/s; PIO 1 MB in %llu ms = %llu KB/s\n",
	    dfe_dma_pa, dma_ms, dma_ms ? 16384000 / dma_ms : 0, pio_ms, pio_ms ? 1024000 / pio_ms : 0);
}
uint64_t df_emmc_sectors;                       /* from the GPT header; /dev/emmc size */
static uint64_t
dfe_le(const uint8_t *p, int n)
{
	uint64_t v = 0;
	while (n--) {
		v = (v << 8) | p[n];
	}
	return v;
}

void df_emmc_probe(void);
void
df_emmc_probe(void)
{
	uint32_t r1 = 0;
	dfe = (volatile uint8_t *)ml_io_map(DFE_BASE, 0x1000);
	dfe_w(E_INTEN, 0);                                  /* polled: no MSDC IRQ */
	dfe_w(E_CFG, dfe_r(E_CFG) | (1u << 3));             /* PIO */
	int rc = dfe_cmd(13, 1u << 16, 0, &r1);              /* SEND_STATUS, RCA 1 */
	printf("emmc: MSDC0 cfg %x, CMD13 %d, state %u (4 = tran)\n", dfe_r(E_CFG), rc, (r1 >> 9) & 15);

	if ((rc = df_emmc_read(1, dfe_blk))) {
		printf("emmc: read LBA 1 failed %d (int %x)\n", rc, dfe_r(E_INT));
		return;
	}
	if (dfe_le(dfe_blk, 8) != 0x5452415020494645ULL) { /* "EFI PART" */
		printf("emmc: LBA 1 has no GPT header (%02x %02x %02x %02x)\n", dfe_blk[0], dfe_blk[1], dfe_blk[2], dfe_blk[3]);
		return;
	}
	uint64_t ent_lba = dfe_le(dfe_blk + 72, 8), last = dfe_le(dfe_blk + 48, 8);
	uint32_t n = (uint32_t)dfe_le(dfe_blk + 80, 4), esz = (uint32_t)dfe_le(dfe_blk + 84, 4);
	printf("emmc: GPT ok, %u entries of %u bytes at LBA %llu, disk %llu MB\n", n, esz, ent_lba,
	    (last + 1) >> 11);
	df_emmc_sectors = last + 1;
	dfe_dma_init(2153744);                              /* benchmark on boot_b (read-only) */
	if (esz != 128 || n > 128) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0 && df_emmc_read((uint32_t)ent_lba + i / 4, dfe_blk)) {
			printf("emmc: read of the entry table failed\n");
			return;
		}
		const uint8_t *en = dfe_blk + (i % 4) * 128;
		uint64_t a = dfe_le(en + 32, 8), b = dfe_le(en + 40, 8);
		if (!a) {
			continue;
		}
		char name[37];
		for (int c = 0; c < 36; c++) {
			name[c] = (char)en[56 + 2 * c];     /* UTF-16LE, ASCII names */
		}
		name[36] = 0;
		printf("emmc: p%-3u %-16s LBA %8llu  %6llu MB\n", i + 1, name, a, (b - a + 1) >> 11);
	}
}

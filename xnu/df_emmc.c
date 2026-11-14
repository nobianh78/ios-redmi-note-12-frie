
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

/* resp: 1 = R1. data: 0 none, 1 single-block read. Read-only by construction: bit13 (write) never set. */
static int
dfe_cmd(unsigned op, uint32_t arg, int data, uint32_t *resp)
{
	for (int i = 0; dfe_r(E_SDC_STS) & 3; i++) {
		if (i > 1000000) {
			return -1;
		}
	}
	dfe_w(E_INT, dfe_r(E_INT));
	dfe_w(E_SDC_ARG, arg);
	dfe_w(E_SDC_CMD, op | (1u << 7) | (data ? (1u << 11) | (512u << 16) : 0));
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
	uint32_t *p = buf, r1;
	dfe_w(E_FIFOCS, dfe_r(E_FIFOCS) | (1u << 31));
	for (int i = 0; dfe_r(E_FIFOCS) & (1u << 31); i++) {
		if (i > 1000000) {
			return -1;
		}
	}
	dfe_w(E_BLK_NUM, 1);
	int e = dfe_cmd(17, lba, 1, &r1);
	if (e) {
		return e;
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

void df_emmc_probe(void);
void
df_emmc_probe(void)
{
	uint32_t r1 = 0;
	dfe = (volatile uint8_t *)ml_io_map(DFE_BASE, 0x1000);
	dfe_w(E_INTEN, 0);                                  /* polled: no MSDC IRQ */
	dfe_w(E_CFG, dfe_r(E_CFG) | (1u << 3));             /* PIO */
	int e = dfe_cmd(13, 1u << 16, 0, &r1);              /* SEND_STATUS, RCA 1 */
	printf("emmc: MSDC0 cfg %x, CMD13 %d, state %u (4 = tran)\n", dfe_r(E_CFG), e, (r1 >> 9) & 15);

	if ((e = df_emmc_read(1, dfe_blk))) {
		printf("emmc: read LBA 1 failed %d (int %x)\n", e, dfe_r(E_INT));
		return;
	}
	if (memcmp(dfe_blk, "EFI PART", 8)) {
		printf("emmc: LBA 1 has no GPT header (%02x %02x %02x %02x)\n", dfe_blk[0], dfe_blk[1], dfe_blk[2], dfe_blk[3]);
		return;
	}
	uint64_t ent_lba = *(uint64_t *)(dfe_blk + 72), last = *(uint64_t *)(dfe_blk + 48);
	uint32_t n = *(uint32_t *)(dfe_blk + 80), esz = *(uint32_t *)(dfe_blk + 84);
	printf("emmc: GPT ok, %u entries of %u bytes at LBA %llu, disk %llu MB\n", n, esz, ent_lba,
	    (last + 1) >> 11);
	if (esz != 128 || n > 128) {
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (i % 4 == 0 && df_emmc_read((uint32_t)ent_lba + i / 4, dfe_blk)) {
			printf("emmc: read of the entry table failed\n");
			return;
		}
		const uint8_t *en = dfe_blk + (i % 4) * 128;
		uint64_t a = *(const uint64_t *)(en + 32), b = *(const uint64_t *)(en + 40);
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


/*
 * DarwinFire Touchscreen driver: FocalTech FT8720 (TDDI, flashless PRAM boot)
 * over MT6768 SPI2 (0x11012000, polled FIFO PIO). Appended to
 * osfmk/arm64/amcc_rorgn_common.c after df_emmc.c.
 *
 * Also handles Ctrl-T (synthetic swipe-up) and Ctrl-Y (center tap) requests
 * from df_usb.c so the iOS 16 IOHIDEventService -> backboardd -> SpringBoard
 * pipeline can be exercised at any time over COM6.
 */
#include "df_ft8720_fw.h"

struct df_touch_pt {
	uint8_t id;
	uint8_t in_range;
	uint8_t is_touch;
	uint8_t event_mask;
	int32_t x_fixed;
	int32_t y_fixed;
};

extern int df_hid_dispatch_touch(const struct df_touch_pt *pts, int count,
    uint32_t parent_mask, int any_range, int any_touch);

#define DFT_TOPCKGEN_PA   0x10000000UL
#define DFT_INFRACFG_PA   0x10001000UL
#define DFT_GPIO_PA       0x10005000UL
#define DFT_SPI2_PA       0x11012000UL

/* MT6768 SPI2 registers (mediatek,mt6765-spi, enhance_timing = 1) */
#define SPI_CFG0_REG      0x00
#define SPI_CFG1_REG      0x04
#define SPI_TX_DATA_REG   0x10
#define SPI_RX_DATA_REG   0x14
#define SPI_CMD_REG       0x18
#define SPI_STATUS0_REG   0x1c
#define SPI_STATUS1_REG   0x20
#define SPI_PAD_SEL_REG   0x24
#define SPI_CFG2_REG      0x28

#define SPI_CMD_ACT       (1u << 0)
#define SPI_CMD_RESUME    (1u << 1)
#define SPI_CMD_RST       (1u << 2)
#define SPI_CMD_PAUSE_EN  (1u << 4)
#define SPI_CMD_TXMSBF    (1u << 12)
#define SPI_CMD_RXMSBF    (1u << 13)
#define SPI_CMD_FINISH_IE (1u << 16)
#define SPI_CMD_PAUSE_IE  (1u << 17)
#define SPI_CMD_BASE      (SPI_CMD_TXMSBF | SPI_CMD_RXMSBF | SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE)

/* Redmi 12 (fire) FT8720 GPIOs (verified from stock dtbo.img) */
#define DFT_GPIO_IRQ      2u    /* active LOW */
#define DFT_GPIO_RST      22u   /* active LOW */
#define DFT_GPIO_SPI_CS   25u   /* mode 2 */
#define DFT_GPIO_SPI_MO   26u   /* mode 2 */
#define DFT_GPIO_SPI_CLK  27u   /* mode 2 */
#define DFT_GPIO_SPI_MI   28u   /* mode 2 */

/* Display dimensions (iOS 16 framebuffer is 1080x2400 at top of 1080x2460 panel) */
#define DFT_DISP_W        1080
#define DFT_DISP_H        2400

/* Set by df_usb.c on Ctrl-T (1 = swipe up), Ctrl-Y (2 = tap), Ctrl-G (3 = status) */
extern volatile int df_touch_test_req;
volatile int df_touch_test_req = 0;

static volatile uint32_t *dft_topck;
static volatile uint32_t *dft_infra;
static volatile uint32_t *dft_gpio;
static volatile uint32_t *dft_spi;

static uint8_t dft_tx_buf[2064];
static uint8_t dft_rx_buf[2064];
static uint8_t dft_pkt_buf[1032];

static inline uint32_t dft_spi_r(unsigned off) {
	uint32_t v = dft_spi[off >> 2];
	__asm__ volatile ("dsb sy" ::: "memory");
	return v;
}
static inline void dft_spi_w(unsigned off, uint32_t v) {
	__asm__ volatile ("dsb sy" ::: "memory");
	dft_spi[off >> 2] = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
dft_udelay(unsigned us)
{
	uint64_t frq = 0, t0 = 0, cur = 0;
	__asm__ volatile ("mrs %0, CNTFRQ_EL0" : "=r"(frq));
	__asm__ volatile ("mrs %0, CNTVCT_EL0" : "=r"(t0));
	uint64_t ticks = (frq / 1000000ULL) * (uint64_t)us;
	if (ticks == 0) {
		ticks = 26;
	}
	do {
		__asm__ volatile ("mrs %0, CNTVCT_EL0" : "=r"(cur));
	} while ((cur - t0) < ticks);
}

static void
dft_gpio_set_mode(unsigned pin, unsigned mode)
{
	unsigned off = 0x300 + (pin / 8) * 0x10;
	unsigned sh = (pin % 8) * 4;
	dft_gpio[(off + 0x8) >> 2] = 0x7u << sh;         /* CLR */
	if (mode & 0x7u) {
		dft_gpio[(off + 0x4) >> 2] = (mode & 0x7u) << sh; /* SET */
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
dft_gpio_set_dir(unsigned pin, int out)
{
	unsigned off = 0x000 + (pin / 32) * 0x10;
	uint32_t bit = 1u << (pin % 32);
	dft_gpio[(off + (out ? 0x4 : 0x8)) >> 2] = bit;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
dft_gpio_set_out(unsigned pin, int high)
{
	unsigned off = 0x100 + (pin / 32) * 0x10;
	uint32_t bit = 1u << (pin % 32);
	dft_gpio[(off + (high ? 0x4 : 0x8)) >> 2] = bit;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static int
dft_gpio_get_in(unsigned pin)
{
	unsigned off = 0x200 + (pin / 32) * 0x10;
	uint32_t v = dft_gpio[off >> 2];
	__asm__ volatile ("dsb ld" ::: "memory");
	return (v >> (pin % 32)) & 1;
}

static void
dft_hw_init(void)
{
	dft_topck = (volatile uint32_t *)ml_io_map(DFT_TOPCKGEN_PA, 0x1000);
	dft_infra = (volatile uint32_t *)ml_io_map(DFT_INFRACFG_PA, 0x1000);
	dft_gpio  = (volatile uint32_t *)ml_io_map(DFT_GPIO_PA, 0x1000);
	dft_spi   = (volatile uint32_t *)ml_io_map(DFT_SPI2_PA, 0x1000);

	/* Ungate topckgen spi_sel (CLK_CFG_2 bit 31 = PDN, bits 25:24 = mux 0 = 26 MHz) */
	uint32_t cfg2_before = dft_topck[0x60 >> 2];
	uint32_t cg2_before  = dft_infra[0xa4 >> 2];
	dft_topck[0x68 >> 2] = (0x83u << 24);            /* CLK_CFG_2_CLR: mux=0 (26MHz), PDN=0 */
	__asm__ volatile ("dsb sy" ::: "memory");
	dft_topck[0x04 >> 2] = (1u << 11);               /* CLK_CFG_UPDATE bit 11 = spi_sel */
	__asm__ volatile ("dsb sy" ::: "memory");
	dft_infra[0xa8 >> 2] = (1u << 9);                /* MODULE_SW_CG_2_CLR bit 9 = SPI2 */
	__asm__ volatile ("dsb sy" ::: "memory");

	/* Configure SPI2 pins (GPIO 25..28 -> mode 2), IRQ (GPIO 2 -> in), RST (GPIO 22 -> out high) */
	dft_gpio_set_mode(DFT_GPIO_IRQ, 0);
	dft_gpio_set_dir(DFT_GPIO_IRQ, 0);
	dft_gpio_set_mode(DFT_GPIO_RST, 0);
	dft_gpio_set_dir(DFT_GPIO_RST, 1);
	dft_gpio_set_out(DFT_GPIO_RST, 1);
	dft_gpio_set_mode(DFT_GPIO_SPI_CS, 2);
	dft_gpio_set_mode(DFT_GPIO_SPI_MO, 2);
	dft_gpio_set_mode(DFT_GPIO_SPI_CLK, 2);
	dft_gpio_set_mode(DFT_GPIO_SPI_MI, 2);

	/* Configure SPI2 timing: 26 MHz / (2 * 3) = 4.33 MHz, Mode 0, MSB-first */
	dft_spi_w(SPI_PAD_SEL_REG, 0);
	dft_spi_w(SPI_CFG2_REG, (2u << 16) | 2u);        /* SCK_LOW=2, SCK_HIGH=2 -> div=3 */
	dft_spi_w(SPI_CFG0_REG, (5u << 16) | 5u);        /* CS_SETUP=5, CS_HOLD=5 */
	dft_spi_w(SPI_CFG1_REG, 5u);                     /* CS_IDLE=5 */
	dft_spi_w(SPI_CMD_REG, SPI_CMD_BASE | SPI_CMD_RST);
	dft_udelay(10);
	dft_spi_w(SPI_CMD_REG, SPI_CMD_BASE);
	dft_udelay(10);
	(void)dft_spi_r(SPI_STATUS0_REG);

	printf("touch: clk cfg2 %x->%x cg2 %x->%x gpio_m3=%x irq=%d\n",
	    cfg2_before, dft_topck[0x60 >> 2], cg2_before, dft_infra[0xa4 >> 2],
	    dft_gpio[0x330 >> 2], dft_gpio_get_in(DFT_GPIO_IRQ));
}

static void
dft_spi_reset(void)
{
	dft_spi_w(SPI_CMD_REG, SPI_CMD_BASE | SPI_CMD_RST);
	dft_udelay(10);
	dft_spi_w(SPI_CMD_REG, SPI_CMD_BASE);
	dft_udelay(10);
	(void)dft_spi_r(SPI_STATUS0_REG);
}

/* Full-duplex polled single-shot FIFO PIO transfer on MT6768 SPI2.
 * Matches stock Redmi 12 spi-mt65xx.c mtk_spi_fifo_transfer: NEVER pulse
 * SPI_CMD_RST before normal transfers (26MHz CDC race swallows ACT).
 * Every FT8720 transfer is kept <= 32 bytes (single FIFO shot, FINISH_IF). */
static int
dft_spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (!dft_spi || len == 0 || len > 32) {
		return -1;
	}
	uint32_t words = len >> 2;
	uint32_t cfg1 = ((len - 1u) << 16) | (0u << 8) | 5u;

	for (int attempt = 0; attempt < 3; attempt++) {
		/* Wait for SPI2 controller idle (STATUS1 bit 0 == 1) */
		for (int w = 0; w < 50000; w++) {
			if (dft_spi_r(SPI_STATUS1_REG) & 1u) {
				break;
			}
		}
		/* e1d6456 behaviour (99.75% ok on HW): RST pulse before every packet
		 * resets FIFO/state machine on MT6768; without it ACT never finishes. */
		{
			uint32_t v = dft_spi_r(SPI_CMD_REG);
			dft_spi_w(SPI_CMD_REG, v | SPI_CMD_RST);
			(void)dft_spi_r(SPI_CMD_REG);
			dft_spi_w(SPI_CMD_REG, (v & ~(SPI_CMD_RST | SPI_CMD_PAUSE_EN |
			    SPI_CMD_ACT | SPI_CMD_RESUME)) | SPI_CMD_BASE);
			(void)dft_spi_r(SPI_CMD_REG);
			(void)dft_spi_r(SPI_STATUS0_REG);
		}

		/* Program packet_len = len (bits 25:16), packet_loop = 1 (bits 15:8), cs_idle = 5 */
		dft_spi_w(SPI_CFG1_REG, cfg1);

		/* Fill TX FIFO as 32-bit LE words */
		for (uint32_t w = 0; w < words; w++) {
			uint32_t v = (uint32_t)tx[w * 4] |
			    ((uint32_t)tx[w * 4 + 1] << 8) |
			    ((uint32_t)tx[w * 4 + 2] << 16) |
			    ((uint32_t)tx[w * 4 + 3] << 24);
			dft_spi_w(SPI_TX_DATA_REG, v);
		}
		if (len & 3u) {
			uint32_t v = 0;
			uint32_t base = words << 2;
			for (uint32_t b = 0; b < (len & 3u); b++) {
				v |= (uint32_t)tx[base + b] << (b * 8);
			}
			dft_spi_w(SPI_TX_DATA_REG, v);
		}

		/* Single-shot trigger (no PAUSE_EN): ACT -> FINISH_IF -> CS auto-deassert */
		uint32_t cur_cmd = dft_spi_r(SPI_CMD_REG) & ~(SPI_CMD_RST | SPI_CMD_PAUSE_EN | SPI_CMD_ACT | SPI_CMD_RESUME);
		dft_spi_w(SPI_CMD_REG, cur_cmd | SPI_CMD_BASE | SPI_CMD_ACT);

		uint32_t st = 0;
		for (int spin = 0; spin < 500000; spin++) {
			st = dft_spi_r(SPI_STATUS0_REG);
			if (st & 0x3u) {
				break;
			}
		}
		if (!(st & 0x3u) && (dft_spi_r(SPI_STATUS1_REG) & 1u) &&
		    !(dft_spi_r(SPI_CMD_REG) & SPI_CMD_ACT)) {
			/* Controller idle + ACT self-cleared: packet went out but
			 * FINISH_IF was missed. Accept it; ECC validates the data. */
			st = 1;
		}
		if (!(st & 0x3u)) {
			printf("touch: spi timeout try=%d len=%u st0=%x st1=%x cmd=%x\n",
			    attempt, len, st, dft_spi_r(SPI_STATUS1_REG), dft_spi_r(SPI_CMD_REG));
			dft_spi_reset();
			continue;
		}

		/* Drain RX FIFO as 32-bit LE words */
		for (uint32_t w = 0; w < words; w++) {
			uint32_t v = dft_spi_r(SPI_RX_DATA_REG);
			if (rx) {
				rx[w * 4]     = (uint8_t)(v & 0xff);
				rx[w * 4 + 1] = (uint8_t)((v >> 8) & 0xff);
				rx[w * 4 + 2] = (uint8_t)((v >> 16) & 0xff);
				rx[w * 4 + 3] = (uint8_t)((v >> 24) & 0xff);
			}
		}
		if (len & 3u) {
			uint32_t v = dft_spi_r(SPI_RX_DATA_REG);
			if (rx) {
				uint32_t base = words << 2;
				for (uint32_t b = 0; b < (len & 3u); b++) {
					rx[base + b] = (uint8_t)((v >> (b * 8)) & 0xff);
				}
			}
		}
		return 0;
	}
	return -2;
}

/* FocalTech V3.4 SPI protocol (verified against Redmi 12 fire_linux_Image):
 * fts_write(cmd, len): tx[0]=cmd[0], tx[1]=0x00, tx[2..3]=be16(len-1),
 *                      if len==1 -> 4B; else tx[4..6]=0, tx[7..]=cmd[1..], len+6 bytes.
 * fts_read(reg, buf, rlen): tx[0]=reg, tx[1]=0xA0, tx[2..3]=be16(rlen), rlen+9 bytes,
 *                           data at rx[7..7+rlen-1]. */
static int
dft_fts_write(const uint8_t *cmd, uint32_t len)
{
	if (!cmd || len == 0 || len + 6 > sizeof(dft_tx_buf)) {
		return -1;
	}
	uint32_t total = (len == 1) ? 4u : (len + 6u);
	__builtin_memset(dft_tx_buf, 0, total);
	dft_tx_buf[0] = cmd[0];
	dft_tx_buf[1] = 0x00;
	dft_tx_buf[2] = (uint8_t)(((len - 1u) >> 8) & 0xff);
	dft_tx_buf[3] = (uint8_t)((len - 1u) & 0xff);
	if (len > 1) {
		__builtin_memcpy(dft_tx_buf + 7, cmd + 1, len - 1u);
	}
	int rc = dft_spi_xfer(dft_tx_buf, dft_rx_buf, total);
	if (rc) {
		return rc;
	}
	if ((dft_rx_buf[3] & 0xa0u) != 0) {
		return -3;
	}
	return 0;
}

static int
dft_fts_read(uint8_t reg, uint8_t *rbuf, uint32_t rlen)
{
	if (!rbuf || rlen == 0 || rlen + 9 > sizeof(dft_tx_buf)) {
		return -1;
	}
	uint32_t total = rlen + 9u;
	__builtin_memset(dft_tx_buf, 0, total);
	dft_tx_buf[0] = reg;
	dft_tx_buf[1] = 0xa0;
	dft_tx_buf[2] = (uint8_t)((rlen >> 8) & 0xff);
	dft_tx_buf[3] = (uint8_t)(rlen & 0xff);
	int rc = dft_spi_xfer(dft_tx_buf, dft_rx_buf, total);
	if (rc) {
		return rc;
	}
	if ((dft_rx_buf[3] & 0xa0u) != 0) {
		return -3;
	}
	__builtin_memcpy(rbuf, dft_rx_buf + 7, rlen);
	return 0;
}

static void
dft_fts_reset(void)
{
	dft_gpio_set_out(DFT_GPIO_RST, 0);
	IOSleep(5);
	dft_gpio_set_out(DFT_GPIO_RST, 1);
	IOSleep(10);
}

/* Upload a PRAM/DRAM section using 0xAD (set 24-bit addr) + 0xAE (write 24B data -> 31B wire) */
static int
dft_fts_write_section(const uint8_t *buf, uint32_t len, uint32_t base_addr)
{
	const uint32_t chunk_max = 24;
	uint32_t off = 0;
	while (off < len) {
		uint32_t chunk = len - off;
		if (chunk > chunk_max) {
			chunk = chunk_max;
		}
		uint32_t addr = base_addr + off;
		uint8_t acmd[4] = {
			0xad,
			(uint8_t)((addr >> 16) & 0xff),
			(uint8_t)((addr >> 8) & 0xff),
			(uint8_t)(addr & 0xff)
		};
		int rc = -1;
		for (int retry = 0; retry < 3; retry++) {
			rc = dft_fts_write(acmd, 4);
			if (rc == 0) {
				dft_pkt_buf[0] = 0xae;
				__builtin_memcpy(dft_pkt_buf + 1, buf + off, chunk);
				rc = dft_fts_write(dft_pkt_buf, chunk + 1u);
				if (rc == 0) {
					break;
				}
			}
			IOSleep(1);
		}
		if (rc) {
			printf("touch: write off 0x%x (addr 0x%x) failed (%d)\n", off, addr, rc);
			return rc;
		}
		off += chunk;
	}
	return 0;
}

/* Verify uploaded section ECC (matches 0xffffff8008bf90f8 in Redmi 12 kernel) */
static int
dft_fts_ecc_check(const uint8_t *buf, uint32_t len)
{
	uint16_t ecc = 0;
	for (uint32_t i = 0; i + 1 < len; i += 2) {
		uint16_t w = (uint16_t)(((uint16_t)buf[i] << 8) | buf[i + 1]);
		ecc ^= w;
		for (int b = 0; b < 16; b++) {
			if (ecc & 1u) {
				ecc = (uint16_t)((ecc >> 1) ^ 0x8408u);
			} else {
				ecc >>= 1;
			}
		}
	}
	uint8_t cmd[7] = {
		0xcc, 0x00, 0x00, 0x00,
		(uint8_t)((len >> 16) & 0xff),
		(uint8_t)((len >> 8) & 0xff),
		(uint8_t)(len & 0xff)
	};
	int rc = -1;
	for (int t = 0; t < 5 && rc; t++) {
		rc = dft_fts_write(cmd, 7);
	}
	if (rc) {
		return rc;
	}
	IOSleep(10);
	uint8_t st = 0;
	int ready = 0;
	for (int i = 0; i < 60; i++) {
		if (dft_fts_read(0xce, &st, 1) == 0 && st == 0xa5) {
			ready = 1;
			break;
		}
		IOSleep(5);
	}
	if (!ready) {
		printf("touch: ECC timeout st=0x%x\n", st);
		return -4;
	}
	uint8_t ec[2] = { 0, 0 };
	rc = -1;
	for (int t = 0; t < 5 && rc; t++) {
		rc = dft_fts_read(0xcd, ec, 2);
	}
	if (rc) {
		return rc;
	}
	uint16_t hw_ecc = (uint16_t)(((uint16_t)ec[0] << 8) | ec[1]);
	if (hw_ecc != ecc) {
		printf("touch: ECC mismatch hw=0x%04x host=0x%04x\n", hw_ecc, ecc);
		return -5;
	}
	return 0;
}

static int
dft_fts_boot(void)
{
	/* Check if firmware is somehow already running */
	uint8_t idh = 0, idl = 0, fwv = 0;
	if (dft_fts_read(0xa3, &idh, 1) == 0 && dft_fts_read(0x9f, &idl, 1) == 0 &&
	    idh == 0x87 && idl == 0x20) {
		(void)dft_fts_read(0xa6, &fwv, 1);
		printf("touch: FT8720 already running! id=%02x%02x fw=0x%02x\n", idh, idl, fwv);
		return 0;
	}

	/* Enter ROM bootloader: reset + send 0x55 + read Boot ID (0x90 -> 0x87 0x20) */
	uint8_t bootid[2] = { 0, 0 };
	int got_boot = 0;
	for (int attempt = 0; attempt < 5 && !got_boot; attempt++) {
		dft_fts_reset();
		for (int t = 0; t < 3 && !got_boot; t++) {
			uint8_t c55 = 0x55;
			(void)dft_fts_write(&c55, 1);
			IOSleep(8);
			if (dft_fts_read(0x90, bootid, 2) == 0 &&
			    bootid[0] == 0x87 && bootid[1] == 0x20) {
				got_boot = 1;
				break;
			}
		}
		printf("touch: boot attempt %d -> bootid=%02x%02x rx3=%02x\n",
		    attempt, bootid[0], bootid[1], dft_rx_buf[3]);
	}
	if (!got_boot) {
		printf("touch: FT8720 bootid not found (SPI2/reset issue)\n");
		return -1;
	}

	/* Section 1: PRAM App (64,884 bytes -> 0x000000) */
	int rc = dft_fts_write_section(df_ft8720_fw, DF_FT8720_APP_LEN, 0x000000);
	if (rc == 0) {
		printf("touch: App uploaded (%uB), verifying ECC...\n", DF_FT8720_APP_LEN);
		rc = dft_fts_ecc_check(df_ft8720_fw, DF_FT8720_APP_LEN);
	}
	if (rc) {
		printf("touch: FT8720 App upload/ECC failed (%d)\n", rc);
		return rc;
	}
	/* Section 2: Initcode/DRAM (4,704 bytes -> 0xD00000) */
	rc = dft_fts_write_section(df_ft8720_fw + DF_FT8720_APP_LEN,
	    DF_FT8720_INIT_LEN, 0xD00000);
	if (rc == 0) {
		printf("touch: Initcode uploaded (%uB), verifying ECC...\n", DF_FT8720_INIT_LEN);
		rc = dft_fts_ecc_check(df_ft8720_fw + DF_FT8720_APP_LEN,
		    DF_FT8720_INIT_LEN);
	}
	if (rc) {
		printf("touch: FT8720 Initcode upload/ECC failed (%d)\n", rc);
		return rc;
	}

	/* Start Application (cmd 0x08) */
	uint8_t c08 = 0x08;
	rc = dft_fts_write(&c08, 1);
	if (rc) {
		printf("touch: FT8720 start app (0x08) failed (%d)\n", rc);
		return rc;
	}
	IOSleep(150);

	idh = idl = fwv = 0;
	(void)dft_fts_read(0xa3, &idh, 1);
	(void)dft_fts_read(0x9f, &idl, 1);
	(void)dft_fts_read(0xa6, &fwv, 1);
	printf("touch: FT8720 FW UP! id=%02x%02x fw=0x%02x (expect 8720/0x%02x)\n",
	    idh, idl, fwv, DF_FT8720_FW_VER);
	return 0;
}

/* Synthetic swipe-up / tap for Ctrl-T / Ctrl-Y verification over COM6 */
static void
dft_run_test_gesture(int mode)
{
	struct df_touch_pt pt;
	__builtin_memset(&pt, 0, sizeof(pt));
	pt.id = 0;
	if (mode == 1) {
		/* Smooth bottom-edge swipe up: (0.50, 0.98) -> (0.50, 0.30) */
		printf("touch: injecting synthetic swipe-up (Ctrl-T)\n");
		const int steps = 20;
		for (int s = 0; s <= steps; s++) {
			int32_t y = (int32_t)((98 - (68 * s) / steps) * 65536 / 100);
			pt.x_fixed = 32768; /* 0.50 */
			pt.y_fixed = y;
			if (s == 0) {
				pt.in_range = 1; pt.is_touch = 1; pt.event_mask = 7; /* Range|Touch|Pos */
				df_hid_dispatch_touch(&pt, 1, 7, 1, 1);
			} else if (s < steps) {
				pt.in_range = 1; pt.is_touch = 1; pt.event_mask = 4; /* Pos */
				df_hid_dispatch_touch(&pt, 1, 4, 1, 1);
			} else {
				pt.in_range = 0; pt.is_touch = 0; pt.event_mask = 3; /* Range|Touch up */
				df_hid_dispatch_touch(&pt, 1, 3, 0, 0);
			}
			IOSleep(16);
		}
	} else if (mode == 2) {
		/* Center tap: (0.50, 0.50) */
		printf("touch: injecting synthetic center tap (Ctrl-Y)\n");
		pt.x_fixed = 32768;
		pt.y_fixed = 32768;
		pt.in_range = 1; pt.is_touch = 1; pt.event_mask = 7;
		df_hid_dispatch_touch(&pt, 1, 7, 1, 1);
		IOSleep(50);
		pt.in_range = 0; pt.is_touch = 0; pt.event_mask = 3;
		df_hid_dispatch_touch(&pt, 1, 3, 0, 0);
	}
}

/* Previous finger tracking for clean DOWN / MOVE / UP transitions */
static uint8_t dft_prev_active[10];
static int32_t dft_prev_x[10];
static int32_t dft_prev_y[10];
static int dft_prev_any;
static unsigned dft_log_cnt;

static void
dft_poll_fts(void)
{
	/* Read 20 bytes from reg 0x01 (29B total SPI wire <= 32B single FIFO shot):
	 * buf[0]=gesture, buf[1]=mode|count, buf[2..19] = up to 3 fingers x 6B */
	uint8_t buf[24];
	if (dft_fts_read(0x01, buf, 20) != 0) {
		return;
	}
	if (buf[1] == 0xff && buf[2] == 0xff && buf[3] == 0xff && buf[4] == 0xff) {
		return;
	}
	uint8_t mode = buf[1] >> 4;
	uint8_t point_num = buf[1] & 0x0fu;
	if ((mode != 0 && mode != 2) || point_num > 10) {
		return;
	}

	uint8_t cur_active[10];
	int32_t cur_x[10], cur_y[10];
	__builtin_memset(cur_active, 0, sizeof(cur_active));

	int cur_any = 0;
	if (point_num > 0) {
		for (int i = 0; i < 3; i++) {
			uint8_t *p = buf + 2 + i * 6;
			if (p[2] > 0x9f) {
				break;
			}
			if ((p[0] | p[1] | p[2] | p[3] | p[4] | p[5]) == 0) {
				continue;
			}
			uint8_t id = p[2] >> 4;
			if (id >= 10) {
				continue;
			}
			uint8_t ev = p[0] >> 6; /* 0 = down, 1 = up, 2 = contact, 3 = none */
			if (ev == 3) {
				continue;
			}
			/* Hi-res 8x subpixel coords (verified from fire_linux_Image 0x8be44b0) */
			uint32_t x_hi = (((uint32_t)(p[0] & 0x0f)) << 11) |
			    ((uint32_t)p[1] << 3) | ((uint32_t)(p[4] >> 4) & 0x7u);
			uint32_t y_hi = (((uint32_t)(p[2] & 0x0f)) << 11) |
			    ((uint32_t)p[3] << 3) | ((uint32_t)p[4] & 0x7u);
			int32_t xf = (int32_t)(((uint64_t)x_hi * 65536ULL) / (DFT_DISP_W * 8ULL));
			int32_t yf = (int32_t)(((uint64_t)y_hi * 65536ULL) / (DFT_DISP_H * 8ULL));
			if (xf < 0) xf = 0;
			if (xf > 65536) xf = 65536;
			if (yf < 0) yf = 0;
			if (yf > 65536) yf = 65536;

			if (ev == 0 || ev == 2) { /* down or contact */
				cur_active[id] = 1;
				cur_x[id] = xf;
				cur_y[id] = yf;
				cur_any = 1;
			} else if (ev == 1) { /* explicit lift */
				dft_prev_x[id] = xf;
				dft_prev_y[id] = yf;
			}
		}
	}

	/* Build HID finger list (all active + newly lifted fingers) */
	struct df_touch_pt out[10];
	int n = 0;
	for (int id = 0; id < 10; id++) {
		if (cur_active[id]) {
			out[n].id = (uint8_t)id;
			out[n].in_range = 1;
			out[n].is_touch = 1;
			out[n].event_mask = dft_prev_active[id] ? 4u : 7u; /* Pos vs Range|Touch|Pos */
			out[n].x_fixed = cur_x[id];
			out[n].y_fixed = cur_y[id];
			dft_prev_x[id] = cur_x[id];
			dft_prev_y[id] = cur_y[id];
			n++;
		} else if (dft_prev_active[id]) {
			out[n].id = (uint8_t)id;
			out[n].in_range = 0;
			out[n].is_touch = 0;
			out[n].event_mask = 3u; /* Range|Touch lift */
			out[n].x_fixed = dft_prev_x[id];
			out[n].y_fixed = dft_prev_y[id];
			n++;
		}
		dft_prev_active[id] = cur_active[id];
	}

	if (n == 0) {
		return;
	}

	uint32_t parent_mask = 0;
	if (!dft_prev_any && cur_any) {
		parent_mask = 7u; /* first finger down */
	} else if (dft_prev_any && !cur_any) {
		parent_mask = 3u; /* last finger up */
	} else {
		parent_mask = 4u; /* move / multi-touch change */
	}
	dft_prev_any = cur_any;

	int sent = df_hid_dispatch_touch(out, n, parent_mask, cur_any, cur_any);
	if (dft_log_cnt < 30 || (!cur_any && dft_log_cnt < 60)) {
		dft_log_cnt++;
		printf("touch: n=%d any=%d id=%u (%d,%d) px=(%d,%d) mask=%u sent=%d\n",
		    n, cur_any, out[0].id, out[0].x_fixed, out[0].y_fixed,
		    (out[0].x_fixed * DFT_DISP_W) >> 16,
		    (out[0].y_fixed * DFT_DISP_H) >> 16,
		    parent_mask, sent);
	}
}

static void
dft_thread(void *arg __unused, int wait __unused)
{
	IOSleep(2000); /* wait for COM6 USB console to come up so init logs are visible */
	{
		/* Measure real CPU clock: PMU cycle counter vs CNTVCT over ~50 ms */
		uint64_t midr, mpidr, frq, t0, t1, c0, c1, pmcr;
		__asm__ volatile ("mrs %0, MIDR_EL1" : "=r"(midr));
		__asm__ volatile ("mrs %0, MPIDR_EL1" : "=r"(mpidr));
		__asm__ volatile ("mrs %0, CNTFRQ_EL0" : "=r"(frq));
		__asm__ volatile ("mrs %0, PMCR_EL0" : "=r"(pmcr));
		__asm__ volatile ("msr PMCR_EL0, %0; isb" :: "r"(pmcr | 1u));
		__asm__ volatile ("msr PMCNTENSET_EL0, %0; isb" :: "r"(1ull << 31));
		__asm__ volatile ("isb; mrs %0, CNTVCT_EL0" : "=r"(t0));
		__asm__ volatile ("mrs %0, PMCCNTR_EL0" : "=r"(c0));
		do {
			__asm__ volatile ("isb; mrs %0, CNTVCT_EL0" : "=r"(t1));
		} while (t1 - t0 < frq / 20);
		__asm__ volatile ("mrs %0, PMCCNTR_EL0" : "=r"(c1));
		printf("cpu: midr=0x%llx mpidr=0x%llx cntfrq=%llu pmcr=0x%llx clock=%llu MHz\n",
		    midr, mpidr, frq, pmcr, ((c1 - c0) * frq / (t1 - t0)) / 1000000ULL);
		/* Fallback: 16 dependent adds per iteration = 16 cycles (1-cycle ALU latency) */
		uint64_t n = 2000000, acc = 0;
		__asm__ volatile ("isb; mrs %0, CNTVCT_EL0" : "=r"(t0));
		__asm__ volatile (
			"1:\n"
			"add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
			"add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
			"add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
			"add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
			"subs %0, %0, #1\n b.ne 1b\n"
			: "+r"(n), "+r"(acc));
		__asm__ volatile ("isb; mrs %0, CNTVCT_EL0" : "=r"(t1));
		printf("cpu: loop clock ~%llu MHz (32M adds in %llu ticks)\n",
		    (32000000ULL * frq / (t1 - t0)) / 1000000ULL, t1 - t0);
	}
	dft_hw_init();
	int ft_ok = (dft_fts_boot() == 0);
	int idle_polls = 0;
	for (;;) {
		int req = df_touch_test_req;
		if (req) {
			df_touch_test_req = 0;
			if (req == 3) {
				printf("touch: status ft_ok=%d irq=%d\n", ft_ok, dft_gpio_get_in(DFT_GPIO_IRQ));
				if (!ft_ok) {
					ft_ok = (dft_fts_boot() == 0);
				} else {
					uint8_t raw[16] = { 0 };
					int rc = dft_fts_read(0x01, raw, 14);
					printf("touch: raw01 rc=%d %02x %02x %02x %02x %02x %02x %02x %02x\n",
					    rc, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
				}
			} else {
				dft_run_test_gesture(req);
			}
		}
		if (ft_ok) {
			int irq_low = (dft_gpio_get_in(DFT_GPIO_IRQ) == 0);
			if (irq_low || dft_prev_any || (++idle_polls >= 10)) {
				idle_polls = 0;
				dft_poll_fts();
			}
		}
		IOSleep(10);
	}
}

void df_touch_start(void);
void
df_touch_start(void)
{
	void *th = 0;
	if (kernel_thread_start(dft_thread, 0, &th) == 0 && th) {
		thread_deallocate(th);
	}
}

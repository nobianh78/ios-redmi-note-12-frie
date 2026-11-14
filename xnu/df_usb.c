
/*
 * fire bring-up: USB CDC-ACM console on the MT6768 USB0 controller (Mentor MUSB core, the one LK uses
 * for fastboot). Appended to osfmk/arm64/amcc_rorgn_common.c by fix-xcode16.sh, after the df_* console
 * code (uses df_ps_puts/df_ps_put, df_wdt).
 *
 * Device mode, polled from a kernel thread every 1 ms; no USB IRQ (masked at the MTK L1 mask and the
 * GIC: sleh_irq only acks device IRQs, so a level IRQ would storm). The host sees VID 1209 / PID 0001
 * ("DarwinFire XNU console", class 02) and Windows binds usbser.sys -> a COM port.
 *   out: every console_write byte (kernel printf + /dev/console) is queued and sent on bulk EP1 IN.
 *   in:  bulk EP2 OUT bytes go to cons_cinput() (the console tty); Ctrl-R (0x12) resets the phone.
 * The watchdog is fed here only while the host has the device configured, so a dead link still lets
 * the phone fall back to Android.
 * shortcut: polled PIO, one packet in flight per endpoint; move to IRQ + DMA if throughput matters.
 * Register map: Mentor MUSB (Linux drivers/usb/musb/musb_regs.h); MTK clears interrupt status by
 * writing 1s (drivers/usb/musb/mediatek.c), so status reads are written back.
 */

#define DFU_BASE        0x11200000UL    /* USB0, from the stock DT (mediatek,mt6768-usb20) */
#define DFU_PHY         0x11cc0000UL    /* generic-tphy-v1; u2 port at +0x800 */
#define DFU_GICD        0x0c000000UL    /* GIC distributor (ADT /arm-io/gic) */
#define DFU_IRQ_SPI     97              /* USB0 SPI (GIC ID 129) */

#define M_FADDR     0x00
#define M_POWER     0x01
#define M_INTRTX    0x02
#define M_INTRRX    0x04
#define M_INTRTXE   0x06
#define M_INTRRXE   0x08
#define M_INTRUSB   0x0a
#define M_INTRUSBE  0x0b
#define M_INDEX     0x0e
#define M_TXMAXP    0x10
#define M_CSR0      0x12    /* EP0 CSR; TXCSR for EP1+ */
#define M_RXMAXP    0x14
#define M_RXCSR     0x16
#define M_COUNT     0x18    /* COUNT0 / RXCOUNT */
#define M_FIFO(n)   (0x20 + 4 * (n))
#define M_DEVCTL    0x60
#define M_TXFIFOSZ  0x62
#define M_RXFIFOSZ  0x63
#define M_TXFIFOADD 0x64
#define M_RXFIFOADD 0x66
#define M_L1INTM    0xa4    /* MTK level-1 interrupt mask */

#define POWER_HSMODE    0x10
#define POWER_HSENAB    0x20
#define POWER_SOFTCONN  0x40
#define INTRUSB_RESET   0x04

#define CSR0_RXPKTRDY   0x0001
#define CSR0_TXPKTRDY   0x0002
#define CSR0_SENTSTALL  0x0004
#define CSR0_DATAEND    0x0008
#define CSR0_SETUPEND   0x0010
#define CSR0_SENDSTALL  0x0020
#define CSR0_SVDRXPKT   0x0040
#define CSR0_SVDSETUPEND 0x0080

#define TXCSR_TXPKTRDY  0x0001
#define TXCSR_FIFONE    0x0002
#define TXCSR_FLUSH     0x0008
#define TXCSR_CLRTOG    0x0040
#define TXCSR_MODE      0x2000
#define RXCSR_RXPKTRDY  0x0001
#define RXCSR_FLUSH     0x0010
#define RXCSR_CLRTOG    0x0080

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void IOSleep(unsigned int ms);
extern void cons_cinput(char ch);
extern int kernel_thread_start(void (*fn)(void *, int), void *arg, void **thread);
extern void thread_deallocate(void *thread);
extern int ml_set_interrupts_enabled(int enable);

static volatile uint8_t *dfu;           /* MUSB registers */
static int dfu_configured, dfu_hs, dfu_seen_reset;

static inline uint8_t  dfu_r8(unsigned o)  { return dfu[o]; }
static inline uint16_t dfu_r16(unsigned o) { return *(volatile uint16_t *)(dfu + o); }
static inline uint32_t dfu_r32(unsigned o) { return *(volatile uint32_t *)(dfu + o); }
static inline void dfu_w8(unsigned o, uint8_t v)   { dfu[o] = v; }
static inline void dfu_w16(unsigned o, uint16_t v) { *(volatile uint16_t *)(dfu + o) = v; }
static inline void dfu_w32(unsigned o, uint32_t v) { *(volatile uint32_t *)(dfu + o) = v; }

static void
dfu_hex(const char *label, uint32_t v)
{
	char b[11];
	df_ps_puts(label);
	b[0] = '0';
	b[1] = 'x';
	for (int i = 0; i < 8; i++) {
		b[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xf];
	}
	b[10] = 0;
	df_ps_puts(b);
	df_ps_put(' ');
}

/* ---- TX ring: filled from any context by df_usb_putc, drained by the poll thread ---- */
#define DFU_RING 65536
static char dfu_ring[DFU_RING];
static unsigned dfu_head, dfu_tail;     /* head = write, tail = read */

void df_usb_putc(char c);
void
df_usb_putc(char c)
{
	int s = ml_set_interrupts_enabled(0);
	unsigned n = (dfu_head + 1) % DFU_RING;
	if (n == dfu_tail) {
		dfu_tail = (dfu_tail + 1) % DFU_RING;   /* full: drop the oldest byte */
	}
	dfu_ring[dfu_head] = c;
	dfu_head = n;
	ml_set_interrupts_enabled(s);
}

/* ---- descriptors (VID 0x1209 / PID 0x0001: pid.codes test IDs) ---- */
static const uint8_t dfu_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1
};
static uint8_t dfu_cfg_desc[67] = {
	9, 2, 67, 0, 2, 1, 0, 0x80, 50,                 /* config: 2 interfaces, bus powered 100 mA */
	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,             /* interface 0: CDC comm, ACM, AT */
	5, 0x24, 0x00, 0x10, 0x01,                      /* CDC header 1.10 */
	5, 0x24, 0x01, 0x00, 0x01,                      /* call management -> data iface 1 */
	4, 0x24, 0x02, 0x02,                            /* ACM: line coding + control line state */
	5, 0x24, 0x06, 0x00, 0x01,                      /* union: master 0, slave 1 */
	7, 5, 0x83, 0x03, 16, 0, 0x10,                  /* EP3 IN interrupt, 16 bytes */
	9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,             /* interface 1: CDC data */
	7, 5, 0x02, 0x02, 0x00, 0x02, 0,                /* EP2 OUT bulk (maxp patched per speed) */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* EP1 IN bulk */
};
static const char *const dfu_strings[] = { 0, "DarwinFire", "DarwinFire XNU console", "fire-0001" };
static uint8_t dfu_line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */

/* ---- EP0 ---- */
static const uint8_t *dfu_ep0_ptr;
static unsigned dfu_ep0_left, dfu_ep0_zlp;
static int dfu_ep0_state;               /* 0 idle, 1 sending IN data, 2 expecting OUT data */
static int dfu_addr_pending = -1;
static uint8_t dfu_ep0_buf[256];

static void
dfu_fifo_write(int ep, const uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		dfu_w8(M_FIFO(ep), p[i]);
	}
}
static void
dfu_fifo_read(int ep, uint8_t *p, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		p[i] = dfu_r8(M_FIFO(ep));
	}
}

static void
dfu_ep0_send_chunk(void)
{
	unsigned n = dfu_ep0_left > 64 ? 64 : dfu_ep0_left;
	dfu_fifo_write(0, dfu_ep0_ptr, n);
	dfu_ep0_ptr += n;
	dfu_ep0_left -= n;
	int last = dfu_ep0_left == 0 && !(n == 64 && dfu_ep0_zlp);
	if (dfu_ep0_left == 0 && n == 64 && dfu_ep0_zlp) {
		dfu_ep0_zlp = 0;                /* next chunk is the zero-length end packet */
	}
	dfu_w16(M_CSR0, CSR0_TXPKTRDY | (last ? CSR0_DATAEND : 0));
	dfu_ep0_state = last ? 0 : 1;
}
static void
dfu_ep0_in(const uint8_t *p, unsigned len, unsigned wlength)
{
	if (len > wlength) {
		len = wlength;
	}
	dfu_ep0_ptr = p;
	dfu_ep0_left = len;
	dfu_ep0_zlp = len < wlength && (len % 64) == 0;
	dfu_w16(M_CSR0, CSR0_SVDRXPKT);
	dfu_ep0_send_chunk();
}
static void
dfu_ep0_ack(void)
{
	dfu_w16(M_CSR0, CSR0_SVDRXPKT | CSR0_DATAEND);
}
static void
dfu_ep0_stall(void)
{
	dfu_w16(M_CSR0, CSR0_SVDRXPKT | CSR0_SENDSTALL);
}

static void
dfu_config_eps(void)
{
	unsigned maxp = dfu_hs ? 512 : 64;
	dfu_w8(M_INDEX, 1);                     /* EP1 IN bulk: FIFO 64..575 */
	dfu_w8(M_TXFIFOSZ, 6);
	dfu_w16(M_TXFIFOADD, 64 / 8);
	dfu_w16(M_TXMAXP, (uint16_t)maxp);
	dfu_w16(M_CSR0, TXCSR_MODE | TXCSR_CLRTOG);   /* FLUSHFIFO is only valid on a non-empty FIFO */
	dfu_w8(M_INDEX, 2);                     /* EP2 OUT bulk: FIFO 576..1087 */
	dfu_w8(M_RXFIFOSZ, 6);
	dfu_w16(M_RXFIFOADD, 576 / 8);
	dfu_w16(M_RXMAXP, (uint16_t)maxp);
	dfu_w16(M_RXCSR, RXCSR_CLRTOG);
	dfu_w8(M_INDEX, 3);                     /* EP3 IN interrupt: FIFO 1088..1151 */
	dfu_w8(M_TXFIFOSZ, 3);
	dfu_w16(M_TXFIFOADD, 1088 / 8);
	dfu_w16(M_TXMAXP, 16);
	dfu_w16(M_CSR0, TXCSR_MODE | TXCSR_CLRTOG);
	dfu_w8(M_INDEX, 0);
}

static void
dfu_setup(const uint8_t *s)
{
	uint8_t type = s[0], req = s[1];
	unsigned value = s[2] | (s[3] << 8), wlength = s[6] | (s[7] << 8);

	if ((type & 0x60) == 0x20) {            /* CDC class requests */
		switch (req) {
		case 0x20:                      /* SET_LINE_CODING: 7 bytes follow */
			dfu_w16(M_CSR0, CSR0_SVDRXPKT);
			dfu_ep0_state = 2;
			return;
		case 0x21:                      /* GET_LINE_CODING */
			dfu_ep0_in(dfu_line_coding, 7, wlength);
			return;
		case 0x22:                      /* SET_CONTROL_LINE_STATE */
		case 0x23:                      /* SEND_BREAK */
			dfu_ep0_ack();
			return;
		}
		dfu_ep0_stall();
		return;
	}
	switch (req) {
	case 0x06: {                            /* GET_DESCRIPTOR */
		unsigned dt = value >> 8, idx = value & 0xff;
		if (dt == 1) {
			dfu_ep0_in(dfu_dev_desc, 18, wlength);
		} else if (dt == 2) {
			unsigned maxp = dfu_hs ? 512 : 64;
			dfu_cfg_desc[57] = dfu_cfg_desc[64] = (uint8_t)maxp;          /* EP2 / EP1 wMaxPacketSize */
			dfu_cfg_desc[58] = dfu_cfg_desc[65] = (uint8_t)(maxp >> 8);
			dfu_ep0_in(dfu_cfg_desc, sizeof(dfu_cfg_desc), wlength);
		} else if (dt == 3 && idx == 0) {
			static const uint8_t langid[4] = { 4, 3, 0x09, 0x04 };
			dfu_ep0_in(langid, 4, wlength);
		} else if (dt == 3 && idx < 4) {
			const char *str = dfu_strings[idx];
			unsigned n = 0;
			while (str[n] && n < 120) {
				dfu_ep0_buf[2 + 2 * n] = (uint8_t)str[n];
				dfu_ep0_buf[3 + 2 * n] = 0;
				n++;
			}
			dfu_ep0_buf[0] = (uint8_t)(2 + 2 * n);
			dfu_ep0_buf[1] = 3;
			dfu_ep0_in(dfu_ep0_buf, 2 + 2 * n, wlength);
		} else {
			dfu_ep0_stall();        /* device qualifier, BOS, ...: not supported */
		}
		return;
	}
	case 0x05:                              /* SET_ADDRESS: applied after the status stage */
		dfu_addr_pending = value & 0x7f;
		dfu_ep0_ack();
		return;
	case 0x09:                              /* SET_CONFIGURATION */
		dfu_config_eps();
		dfu_configured = value != 0;
		dfu_ep0_ack();
		df_ps_puts(dfu_configured ? "fire usb: configured\n" : "fire usb: unconfigured\n");
		return;
	case 0x08: {                            /* GET_CONFIGURATION */
		static uint8_t c;
		c = (uint8_t)dfu_configured;
		dfu_ep0_in(&c, 1, wlength);
		return;
	}
	case 0x00: {                            /* GET_STATUS */
		static const uint8_t z[2] = { 0, 0 };
		dfu_ep0_in(z, 2, wlength);
		return;
	}
	case 0x0a: {                            /* GET_INTERFACE */
		static const uint8_t z = 0;
		dfu_ep0_in(&z, 1, wlength);
		return;
	}
	case 0x01:                              /* CLEAR_FEATURE */
	case 0x03:                              /* SET_FEATURE */
	case 0x0b:                              /* SET_INTERFACE */
		dfu_ep0_ack();
		return;
	}
	dfu_ep0_stall();
}

static void
dfu_ep0_irq(void)
{
	dfu_w8(M_INDEX, 0);
	uint16_t csr = dfu_r16(M_CSR0);

	if (dfu_addr_pending >= 0) {            /* status stage of SET_ADDRESS done */
		dfu_w8(M_FADDR, (uint8_t)dfu_addr_pending);
		dfu_addr_pending = -1;
	}
	if (csr & CSR0_SENTSTALL) {
		dfu_w16(M_CSR0, csr & (uint16_t)~CSR0_SENTSTALL);
		dfu_ep0_state = 0;
		return;
	}
	if (csr & CSR0_SETUPEND) {
		dfu_w16(M_CSR0, CSR0_SVDSETUPEND);
		dfu_ep0_state = 0;
	}
	if (csr & CSR0_RXPKTRDY) {
		unsigned n = dfu_r16(M_COUNT) & 0x7f;
		if (dfu_ep0_state == 2) {       /* SET_LINE_CODING data */
			uint8_t tmp[64];
			dfu_fifo_read(0, tmp, n);
			for (unsigned i = 0; i < n && i < 7; i++) {
				dfu_line_coding[i] = tmp[i];
			}
			dfu_ep0_ack();
			dfu_ep0_state = 0;
			return;
		}
		uint8_t setup[8] = { 0 };
		if (n != 8) {
			uint8_t junk[64];
			dfu_fifo_read(0, junk, n);
			dfu_ep0_stall();
			return;
		}
		dfu_fifo_read(0, setup, 8);
		dfu_setup(setup);
		return;
	}
	if (dfu_ep0_state == 1 && !(csr & CSR0_TXPKTRDY)) {
		dfu_ep0_send_chunk();
	}
}

static void
dfu_bulk(void)
{
	/* OUT: host -> console input */
	dfu_w8(M_INDEX, 2);
	uint16_t rx = dfu_r16(M_RXCSR);
	if (rx & RXCSR_RXPKTRDY) {
		uint8_t buf[512];
		unsigned n = dfu_r16(M_COUNT) & 0x1fff;
		if (n > sizeof(buf)) {
			n = sizeof(buf);
		}
		dfu_fifo_read(2, buf, n);
		dfu_w16(M_RXCSR, rx & (uint16_t)~RXCSR_RXPKTRDY);
		dfu_w8(M_INDEX, 0);
		for (unsigned i = 0; i < n; i++) {
			if (buf[i] == 0x12) {           /* Ctrl-R: reset the phone (TOPRGU WDT_SWRST) */
				df_ps_puts("fire usb: Ctrl-R reset\n");
				if (df_wdt) {
					df_wdt[0x14 / 4] = 0x1209;
				}
			}
			cons_cinput(buf[i] == '\r' ? '\n' : (char)buf[i]);
		}
	}
	/* IN: console output -> host, one packet at a time */
	dfu_w8(M_INDEX, 1);
	uint16_t tx = dfu_r16(M_CSR0);
	if (!(tx & TXCSR_TXPKTRDY) && dfu_head != dfu_tail) {
		unsigned maxp = dfu_hs ? 512 : 64, n = 0;
		int s = ml_set_interrupts_enabled(0);
		while (n < maxp && dfu_tail != dfu_head) {
			dfu_w8(M_FIFO(1), (uint8_t)dfu_ring[dfu_tail]);
			dfu_tail = (dfu_tail + 1) % DFU_RING;
			n++;
		}
		ml_set_interrupts_enabled(s);
		dfu_w16(M_CSR0, (uint16_t)(tx | TXCSR_TXPKTRDY));
	}
	dfu_w8(M_INDEX, 0);
}

static void
dfu_poll(void)
{
	uint8_t usb = dfu_r8(M_INTRUSB);
	uint16_t txi = dfu_r16(M_INTRTX);
	uint16_t rxi = dfu_r16(M_INTRRX);
	dfu_w8(M_INTRUSB, usb);                 /* MTK: write-1-to-clear */
	dfu_w16(M_INTRTX, txi);
	dfu_w16(M_INTRRX, rxi);

	if (usb & INTRUSB_RESET) {
		dfu_hs = (dfu_r8(M_POWER) & POWER_HSMODE) != 0;
		dfu_configured = 0;
		dfu_ep0_state = 0;
		dfu_addr_pending = -1;
		dfu_w8(M_FADDR, 0);
		if (dfu_seen_reset++ < 4) {
			df_ps_puts(dfu_hs ? "fire usb: bus reset (high speed)\n" : "fire usb: bus reset (full speed)\n");
		}
	}
	if (txi & 1) {
		dfu_ep0_irq();
	}
	if (dfu_configured) {
		dfu_bulk();
	}
}

static void
dfu_thread(void *arg, int wr)
{
	(void)arg;
	(void)wr;
	unsigned ticks = 0;
	for (;;) {
		dfu_poll();
		if (dfu_configured && df_wdt) {
			df_wdt[0x08 / 4] = 0x1971;      /* host attached: keep the watchdog fed */
		}
		if ((++ticks % 5000) == 1) {
			dfu_hex("fire usb: power", dfu_r8(M_POWER));
			dfu_hex("devctl", dfu_r8(M_DEVCTL));
			dfu_hex("faddr", dfu_r8(M_FADDR));
			df_ps_puts(dfu_configured ? "configured\n" : "not configured\n");
		}
		IOSleep(1);
	}
}

/* Called once the kernel can run threads (just before PID 1 is launched). */
void df_usb_start(void);
void
df_usb_start(void)
{
	dfu = (volatile uint8_t *)ml_io_map(DFU_BASE, 0x4000);
	volatile uint32_t *phy = (volatile uint32_t *)ml_io_map(DFU_PHY, 0x4000);
	volatile uint32_t *gicd = (volatile uint32_t *)ml_io_map(DFU_GICD, 0x4000);
	if (!dfu) {
		df_ps_puts("fire usb: io_map failed\n");
		return;
	}
	/* state LK left behind (fastboot), for the pstore log */
	df_ps_puts("fire usb: LK state ");
	dfu_hex("power", dfu_r8(M_POWER));
	dfu_hex("devctl", dfu_r8(M_DEVCTL));
	dfu_hex("faddr", dfu_r8(M_FADDR));
	dfu_hex("intrusbe", dfu_r8(M_INTRUSBE));
	dfu_hex("intrtxe", dfu_r16(M_INTRTXE));
	dfu_hex("l1intm", dfu_r32(M_L1INTM));
	df_ps_put('\n');
	if (phy) {
		df_ps_puts("fire usb: u2phy ");
		for (int i = 0; i < 32; i++) {
			dfu_hex("", phy[(0x800 / 4) + i]);
		}
		df_ps_put('\n');
	}

	/* no USB IRQ: polled (see the header comment) */
	dfu_w32(M_L1INTM, 0);
	if (gicd) {
		gicd[(0x180 / 4) + ((DFU_IRQ_SPI + 32) / 32)] = 1u << ((DFU_IRQ_SPI + 32) % 32);   /* ICENABLER */
	}

	/* drop off the bus, clean up after LK, come back as the CDC-ACM device */
	dfu_w8(M_POWER, (uint8_t)(dfu_r8(M_POWER) & ~POWER_SOFTCONN));
	IOSleep(300);
	dfu_w8(M_FADDR, 0);
	dfu_w8(M_INDEX, 0);
	dfu_w16(M_INTRTXE, 0x000f);             /* status latches; the line itself stays masked */
	dfu_w16(M_INTRRXE, 0x0004);
	dfu_w8(M_INTRUSBE, 0x07);
	dfu_w8(M_INTRUSB, dfu_r8(M_INTRUSB));
	dfu_w16(M_INTRTX, dfu_r16(M_INTRTX));
	dfu_w16(M_INTRRX, dfu_r16(M_INTRRX));
	dfu_w8(M_POWER, POWER_HSENAB | POWER_SOFTCONN);
	df_ps_puts("fire usb: soft-connect\n");

	void *th = 0;
	if (kernel_thread_start(dfu_thread, 0, &th) == 0 && th) {
		thread_deallocate(th);
	} else {
		df_ps_puts("fire usb: thread start failed\n");
	}
}

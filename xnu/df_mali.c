/*
 * DarwinFire: Mali GPU bring-up, phase 1 (power on + probe) for the Redmi 12 "fire" (MT6768).
 * Appended to osfmk/arm64/amcc_rorgn_common.c by fix-xcode16.sh, after df_touch.c.
 * Run on demand only (COM6 Ctrl-P -> df_touch thread -> df_mali_probe()) so a bus hang while the
 * GPU is unclocked can never break the normal boot.
 *
 * Sequence from the MT6768 vendor kernel (drivers/clk/mediatek/clk-mt6768-pg.c / clk-mt6768.c):
 *   topckgen mfg_sel = syspll_d3 (CLK_CFG_1[1:0] = 2, PDN bit 7 clear, update bit 4)
 *   SPM POWERON_CONFIG_EN = 0xB16 << 16 | 1
 *   MFG_ASYNC (0x334) on -> MFG (0x338) on -> MFG SRAM_PDN off -> bus protect release
 *   -> MFG_CORE0 (0x34C) on + SRAM -> MFG_CORE1 (0x310) on + SRAM -> mfgcfg MFG_CG_CLR = 1
 * then Mali registers (kbase map): GPU_ID, features, soft reset, L2/tiler/shader power-up.
 */

#define DFM_TOPCK_PA    0x10000000ULL
#define DFM_INFRA_PA    0x10001000ULL
#define DFM_SPM_PA      0x10006000ULL
#define DFM_MFGCFG_PA   0x13000000ULL
#define DFM_GPU_PA      0x13040000ULL

#define SPM_PWR_STATUS      0x180
#define SPM_PWR_STATUS_2ND  0x184
#define SPM_MFG_CORE1_CON   0x310
#define SPM_MFG_ASYNC_CON   0x334
#define SPM_MFG_CON         0x338
#define SPM_MFG_CORE0_CON   0x34C

#define PWR_RST_B    (1u << 0)
#define PWR_ISO      (1u << 1)
#define PWR_ON       (1u << 2)
#define PWR_ON_2ND   (1u << 3)
#define PWR_CLK_DIS  (1u << 4)
#define SRAM_PDN     (1u << 8)
#define SRAM_ACK     (1u << 12)

#define GPU_ID            0x000
#define GPU_L2_FEATURES   0x004
#define GPU_CORE_FEATURES 0x008
#define GPU_TILER_FEAT    0x00C
#define GPU_MEM_FEATURES  0x010
#define GPU_MMU_FEATURES  0x014
#define GPU_AS_PRESENT    0x018
#define GPU_JS_PRESENT    0x01C
#define GPU_IRQ_RAWSTAT   0x020
#define GPU_IRQ_CLEAR     0x024
#define GPU_IRQ_MASK      0x028
#define GPU_COMMAND       0x030
#define GPU_STATUS        0x034
#define SHADER_PRESENT_LO 0x100
#define TILER_PRESENT_LO  0x110
#define L2_PRESENT_LO     0x120
#define SHADER_READY_LO   0x140
#define TILER_READY_LO    0x150
#define L2_READY_LO       0x160
#define SHADER_PWRON_LO   0x180
#define TILER_PWRON_LO    0x190
#define L2_PWRON_LO       0x1A0
#define RESET_COMPLETED   (1u << 8)

static volatile uint32_t *dfm_topck, *dfm_infra, *dfm_spm, *dfm_mfgcfg, *dfm_gpu;
static int dfm_powered;

static inline uint32_t dfm_r(volatile uint32_t *b, unsigned off) {
	uint32_t v = b[off >> 2];
	__asm__ volatile ("dsb sy" ::: "memory");
	return v;
}
static inline void dfm_w(volatile uint32_t *b, unsigned off, uint32_t v) {
	__asm__ volatile ("dsb sy" ::: "memory");
	b[off >> 2] = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
dfm_udelay(unsigned us)
{
	uint64_t frq = 0, t0 = 0, cur = 0;
	__asm__ volatile ("mrs %0, CNTFRQ_EL0" : "=r"(frq));
	__asm__ volatile ("mrs %0, CNTVCT_EL0" : "=r"(t0));
	uint64_t ticks = (frq / 1000000ULL) * (uint64_t)us + 1;
	do {
		__asm__ volatile ("mrs %0, CNTVCT_EL0" : "=r"(cur));
	} while ((cur - t0) < ticks);
}

/* wait until (reg & mask) == want, up to ~timeout_us; returns 0 on success */
static int
dfm_wait(volatile uint32_t *b, unsigned off, uint32_t mask, uint32_t want, unsigned timeout_us)
{
	for (unsigned i = 0; i < timeout_us; i += 10) {
		if ((dfm_r(b, off) & mask) == want) {
			return 0;
		}
		dfm_udelay(10);
	}
	return -1;
}

static int
dfm_mtcmos_on(const char *name, unsigned con, uint32_t sta, int sram)
{
	if ((dfm_r(dfm_spm, SPM_PWR_STATUS) & sta) && (dfm_r(dfm_spm, SPM_PWR_STATUS_2ND) & sta) &&
	    (dfm_r(dfm_spm, con) & PWR_RST_B)) {
		printf("mali: %s already on (con=0x%x)\n", name, dfm_r(dfm_spm, con));
		return 0;
	}
	dfm_w(dfm_spm, con, dfm_r(dfm_spm, con) | PWR_ON);
	dfm_w(dfm_spm, con, dfm_r(dfm_spm, con) | PWR_ON_2ND);
	int e1 = dfm_wait(dfm_spm, SPM_PWR_STATUS, sta, sta, 20000);
	int e2 = dfm_wait(dfm_spm, SPM_PWR_STATUS_2ND, sta, sta, 20000);
	dfm_w(dfm_spm, con, dfm_r(dfm_spm, con) & ~PWR_CLK_DIS);
	dfm_w(dfm_spm, con, dfm_r(dfm_spm, con) & ~PWR_ISO);
	dfm_w(dfm_spm, con, dfm_r(dfm_spm, con) | PWR_RST_B);
	int e3 = 0;
	if (sram) {
		dfm_w(dfm_spm, con, dfm_r(dfm_spm, con) & ~SRAM_PDN);
		e3 = dfm_wait(dfm_spm, con, SRAM_ACK, 0, 20000);
	}
	printf("mali: %s on con=0x%x sta=%d sta2=%d sram=%d\n", name, dfm_r(dfm_spm, con), e1, e2, e3);
	return (e1 || e2 || e3) ? -1 : 0;
}

static int
dfm_power_on(void)
{
	if (!dfm_topck) {
		dfm_topck = (volatile uint32_t *)ml_io_map(DFM_TOPCK_PA, 0x1000);
		dfm_infra = (volatile uint32_t *)ml_io_map(DFM_INFRA_PA, 0x1000);
		dfm_spm = (volatile uint32_t *)ml_io_map(DFM_SPM_PA, 0x1000);
		dfm_mfgcfg = (volatile uint32_t *)ml_io_map(DFM_MFGCFG_PA, 0x1000);
		dfm_gpu = (volatile uint32_t *)ml_io_map(DFM_GPU_PA, 0x4000);
	}
	if (!dfm_topck || !dfm_infra || !dfm_spm || !dfm_mfgcfg || !dfm_gpu) {
		printf("mali: io map failed\n");
		return -1;
	}
	/* GPU clock: mfg_sel <- syspll_d3 (mainpll/3, always running), ungate the mux */
	uint32_t cfg1 = dfm_r(dfm_topck, 0x50);
	dfm_w(dfm_topck, 0x58, 0x83);           /* CLK_CFG_1_CLR: mux[1:0] + PDN bit 7 */
	dfm_w(dfm_topck, 0x54, 0x02);           /* CLK_CFG_1_SET: syspll_d3 */
	dfm_w(dfm_topck, 0x04, 1u << 4);        /* CLK_CFG_UPDATE: mfg_sel */
	printf("mali: mfg_sel cfg1 0x%x -> 0x%x\n", cfg1, dfm_r(dfm_topck, 0x50));

	dfm_w(dfm_spm, 0x000, (0xB16u << 16) | 1u);   /* POWERON_CONFIG_EN */
	printf("mali: spm pwr_status 0x%x/0x%x\n", dfm_r(dfm_spm, SPM_PWR_STATUS),
	    dfm_r(dfm_spm, SPM_PWR_STATUS_2ND));

	int err = 0;
	err |= dfm_mtcmos_on("MFG_ASYNC", SPM_MFG_ASYNC_CON, 1u << 14, 0);
	err |= dfm_mtcmos_on("MFG", SPM_MFG_CON, 1u << 11, 1);
	/* release MFG bus protect (step2 then step1), INFRA_TOPAXI_PROTECTEN_CLR */
	dfm_w(dfm_infra, 0x2A4, (1u << 21) | (1u << 22));
	dfm_w(dfm_infra, 0x2A4, (1u << 25));
	printf("mali: bus protect sta1 0x%x\n", dfm_r(dfm_infra, 0x228));
	err |= dfm_mtcmos_on("MFG_CORE0", SPM_MFG_CORE0_CON, 1u << 12, 1);
	err |= dfm_mtcmos_on("MFG_CORE1", SPM_MFG_CORE1_CON, 1u << 13, 1);

	dfm_w(dfm_mfgcfg, 0x008, 0x1);          /* MFG_CG_CLR: bg3d clock on */
	printf("mali: mfg_cg_con 0x%x\n", dfm_r(dfm_mfgcfg, 0x000));
	return err;
}

/* Called from the df_touch thread on COM6 Ctrl-P. */
void df_mali_probe(void);
void
df_mali_probe(void)
{
	printf("mali: === probe start ===\n");
	if (!dfm_powered) {
		int e = dfm_power_on();
		dfm_powered = 1;
		if (e) {
			printf("mali: power sequence reported errors, probing GPU anyway\n");
		}
	}
	uint32_t id = dfm_r(dfm_gpu, GPU_ID);
	printf("mali: GPU_ID 0x%08x (arch %u.%u prod 0x%x rev r%up%u)\n", id,
	    id >> 28, (id >> 24) & 0xf, id >> 16, (id >> 12) & 0xf, (id >> 4) & 0xf);
	printf("mali: L2_FEAT 0x%x CORE_FEAT 0x%x TILER_FEAT 0x%x MEM_FEAT 0x%x MMU_FEAT 0x%x\n",
	    dfm_r(dfm_gpu, GPU_L2_FEATURES), dfm_r(dfm_gpu, GPU_CORE_FEATURES),
	    dfm_r(dfm_gpu, GPU_TILER_FEAT), dfm_r(dfm_gpu, GPU_MEM_FEATURES),
	    dfm_r(dfm_gpu, GPU_MMU_FEATURES));
	uint32_t shaders = dfm_r(dfm_gpu, SHADER_PRESENT_LO);
	uint32_t tilers = dfm_r(dfm_gpu, TILER_PRESENT_LO);
	uint32_t l2 = dfm_r(dfm_gpu, L2_PRESENT_LO);
	printf("mali: AS_PRESENT 0x%x JS_PRESENT 0x%x SHADER 0x%x TILER 0x%x L2 0x%x\n",
	    dfm_r(dfm_gpu, GPU_AS_PRESENT), dfm_r(dfm_gpu, GPU_JS_PRESENT), shaders, tilers, l2);
	if (id == 0 || id == 0xffffffffu) {
		printf("mali: GPU not responding, stop\n");
		return;
	}

	/* soft reset */
	dfm_w(dfm_gpu, GPU_IRQ_MASK, 0);
	dfm_w(dfm_gpu, GPU_IRQ_CLEAR, 0xffffffffu);
	dfm_w(dfm_gpu, GPU_COMMAND, 1);         /* SOFT_RESET */
	int r = dfm_wait(dfm_gpu, GPU_IRQ_RAWSTAT, RESET_COMPLETED, RESET_COMPLETED, 100000);
	printf("mali: soft reset %s rawstat 0x%x status 0x%x\n", r ? "TIMEOUT" : "ok",
	    dfm_r(dfm_gpu, GPU_IRQ_RAWSTAT), dfm_r(dfm_gpu, GPU_STATUS));
	dfm_w(dfm_gpu, GPU_IRQ_CLEAR, 0xffffffffu);

	/* power up L2 -> tiler -> shader cores */
	dfm_w(dfm_gpu, L2_PWRON_LO, l2);
	int e1 = dfm_wait(dfm_gpu, L2_READY_LO, l2, l2, 100000);
	dfm_w(dfm_gpu, TILER_PWRON_LO, tilers);
	int e2 = dfm_wait(dfm_gpu, TILER_READY_LO, tilers, tilers, 100000);
	dfm_w(dfm_gpu, SHADER_PWRON_LO, shaders);
	int e3 = dfm_wait(dfm_gpu, SHADER_READY_LO, shaders, shaders, 100000);
	printf("mali: READY L2 0x%x(%s) TILER 0x%x(%s) SHADER 0x%x(%s)\n",
	    dfm_r(dfm_gpu, L2_READY_LO), e1 ? "timeout" : "ok",
	    dfm_r(dfm_gpu, TILER_READY_LO), e2 ? "timeout" : "ok",
	    dfm_r(dfm_gpu, SHADER_READY_LO), e3 ? "timeout" : "ok");
	printf("mali: === probe done (%s) ===\n", (e1 || e2 || e3) ? "partial" : "GPU UP");
	if (!(e1 || e2 || e3)) {
		extern void df_mali_hello(void);
		df_mali_hello();
	}
}

/* ---------------- Phase 2: MMU AS0 + job manager hello world ---------------- */
/*
 * Mali-G52 has the AArch64 MMU (HW_FEATURE_AARCH64_MMU): standard ARMv8 stage-1 4K tables,
 * as Mesa panfrost_mmu.c uses (io-pgtable ARM_64_LPAE_S1). VA 35 bits -> walk starts at L1.
 * Identity map (GPU VA == PA) with 2 MB L2 blocks. All GPU memory attributes non-cacheable
 * (AS_MEMATTR 0x4C), so the CPU cleans its caches before a job and invalidates after.
 */
#define MMU_IRQ_RAWSTAT   0x2000
#define MMU_IRQ_CLEAR     0x2004
#define AS0               0x2400
#define AS_TRANSTAB_LO    0x00
#define AS_TRANSTAB_HI    0x04
#define AS_MEMATTR_LO     0x08
#define AS_MEMATTR_HI     0x0C
#define AS_COMMAND        0x18
#define AS_FAULTSTATUS    0x1C
#define AS_FAULTADDR_LO   0x20
#define AS_FAULTADDR_HI   0x24
#define AS_STATUS         0x28
#define AS_TRANSCFG_LO    0x30
#define AS_TRANSCFG_HI    0x34
#define JOB_IRQ_RAWSTAT   0x1000
#define JOB_IRQ_CLEAR     0x1004
#define JS1               (0x1800 + 1 * 0x80)
#define JS_HEAD_LO        0x00
#define JS_HEAD_HI        0x04
#define JS_STATUS         0x24
#define JS_HEAD_NEXT_LO   0x40
#define JS_HEAD_NEXT_HI   0x44
#define JS_AFFINITY_NEXT_LO 0x50
#define JS_AFFINITY_NEXT_HI 0x54
#define JS_CONFIG_NEXT    0x58
#define JS_COMMAND_NEXT   0x60

extern vm_offset_t ml_vtophys(vm_offset_t);
extern uint64_t mach_absolute_time(void);

/* one 16 KB kernel page, physically contiguous: L1 | L2 A | L2 B | jobs + results */
static uint8_t dfm_mem[0x8000] __attribute__((aligned(0x4000)));
static uint8_t dfm_heap[0x40000] __attribute__((aligned(0x1000)));   /* tiler heap */

static void
dfm_clean(const void *p, uint64_t len)
{
	uintptr_t a = (uintptr_t)p & ~63UL, e = (uintptr_t)p + len;
	for (; a < e; a += 64) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static int
dfm_as_cmd(uint32_t cmd)
{
	dfm_w(dfm_gpu, AS0 + AS_COMMAND, cmd);
	return dfm_wait(dfm_gpu, AS0 + AS_STATUS, 1, 0, 100000);   /* AS_ACTIVE clear */
}

static void
dfm_map_gb(uint64_t *l1, uint64_t *l2, uint64_t l2_pa, uint64_t pa_any)
{
	uint64_t gb = pa_any >> 30;
	l1[gb] = l2_pa | 3;                              /* table descriptor */
	for (uint64_t i = 0; i < 512; i++) {
		uint64_t pa = (gb << 30) | (i << 21);
		/* block: valid, AttrIdx 0, AP[1]=1 (EL0 RW), AF, nG */
		l2[i] = pa | 1 | (1ULL << 6) | (1ULL << 10) | (1ULL << 11);
	}
}

void df_mali_hello(void);
void
df_mali_hello(void)
{
	uint64_t base_pa = ml_vtophys((vm_offset_t)dfm_mem);
	uint64_t *l1 = (uint64_t *)(dfm_mem + 0x0000);
	uint64_t *l2a = (uint64_t *)(dfm_mem + 0x1000);
	uint64_t *l2b = (uint64_t *)(dfm_mem + 0x2000);
	uint8_t *jobs = dfm_mem + 0x3000;
	volatile uint64_t *result = (volatile uint64_t *)(dfm_mem + 0x3800);
	printf("mali: hello: page va %p pa 0x%llx\n", dfm_mem, base_pa);
	if (!base_pa) {
		return;
	}
	for (unsigned i = 0; i < sizeof(dfm_mem); i++) {
		dfm_mem[i] = 0;
	}
	dfm_map_gb(l1, l2a, base_pa + 0x1000, base_pa);                 /* our page's GB */
	if ((0x7ce6a000ULL >> 30) != (base_pa >> 30)) {
		dfm_map_gb(l1, l2b, base_pa + 0x2000, 0x7ce6a000ULL);   /* framebuffer GB */
	}
	*result = 0x1111111111111111ULL;

	/* WRITE_VALUE job: 32 B header + payload (address, type=Immediate 64, value) */
	uint32_t *h = (uint32_t *)jobs;
	h[4] = (1u << 0) | (2u << 1);       /* +0x10: 64-bit descriptor, job_type WRITE_VALUE=2 */
	h[4] |= (1u << 16);                 /* +0x12: job_index = 1 */
	uint64_t *pl = (uint64_t *)(jobs + 0x20);
	pl[0] = base_pa + 0x3800;           /* address (GPU VA == PA) */
	pl[1] = 7;                          /* type: Immediate 64 */
	pl[2] = 0xCAFEBABE12345678ULL;      /* value */
	dfm_clean(dfm_mem, sizeof(dfm_mem));

	/* AS0: tables, memattr (all non-cacheable), AArch64 4K, 35-bit input addresses */
	uint64_t transcfg = 6ULL /* ADRMODE_AARCH64_4K */ | ((uint64_t)(55 - 35) << 6) /* INA_BITS */ |
	    (1ULL << 24) /* PTW non-cacheable */;
	dfm_w(dfm_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	dfm_w(dfm_gpu, AS0 + AS_TRANSTAB_LO, (uint32_t)base_pa);
	dfm_w(dfm_gpu, AS0 + AS_TRANSTAB_HI, (uint32_t)(base_pa >> 32));
	dfm_w(dfm_gpu, AS0 + AS_MEMATTR_LO, 0x4C4C4C4Cu);
	dfm_w(dfm_gpu, AS0 + AS_MEMATTR_HI, 0x4C4C4C4Cu);
	dfm_w(dfm_gpu, AS0 + AS_TRANSCFG_LO, (uint32_t)transcfg);
	dfm_w(dfm_gpu, AS0 + AS_TRANSCFG_HI, (uint32_t)(transcfg >> 32));
	int ea = dfm_as_cmd(1 /* UPDATE */);
	printf("mali: AS0 update %s transcfg 0x%llx status 0x%x\n", ea ? "TIMEOUT" : "ok",
	    transcfg, dfm_r(dfm_gpu, AS0 + AS_STATUS));

	/* submit on job slot 1, AS0, flush clean+invalidate at start and end */
	uint64_t t0 = mach_absolute_time();
	dfm_w(dfm_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	uint64_t job_va = base_pa + 0x3000;
	dfm_w(dfm_gpu, JS1 + JS_HEAD_NEXT_LO, (uint32_t)job_va);
	dfm_w(dfm_gpu, JS1 + JS_HEAD_NEXT_HI, (uint32_t)(job_va >> 32));
	dfm_w(dfm_gpu, JS1 + JS_AFFINITY_NEXT_LO, 0x3);
	dfm_w(dfm_gpu, JS1 + JS_AFFINITY_NEXT_HI, 0);
	dfm_w(dfm_gpu, JS1 + JS_CONFIG_NEXT, 0u /* AS0 */ | (3u << 8) | (3u << 12) | (8u << 16));
	dfm_w(dfm_gpu, JS1 + JS_COMMAND_NEXT, 1 /* START */);
	uint32_t raw = 0;
	for (int i = 0; i < 20000; i++) {          /* ~200 ms: slot 1 DONE (bit 1) or FAIL (bit 17) */
		raw = dfm_r(dfm_gpu, JOB_IRQ_RAWSTAT);
		if (raw & ((1u << 1) | (1u << 17))) {
			break;
		}
		dfm_udelay(10);
	}
	uint64_t t1 = mach_absolute_time();
	dfm_clean((const void *)result, 64);
	dfm_clean(jobs, 64);
	printf("mali: job raw 0x%x js1 status 0x%x head 0x%x exc 0x%x | result 0x%llx (%s) in %llu ticks\n",
	    raw, dfm_r(dfm_gpu, JS1 + JS_STATUS), dfm_r(dfm_gpu, JS1 + JS_HEAD_LO), h[0],
	    *result, *result == 0xCAFEBABE12345678ULL ? "GPU WROTE IT" : "no write", t1 - t0);
	printf("mali: mmu raw 0x%x fault 0x%x addr 0x%x%08x\n", dfm_r(dfm_gpu, MMU_IRQ_RAWSTAT),
	    dfm_r(dfm_gpu, AS0 + AS_FAULTSTATUS), dfm_r(dfm_gpu, AS0 + AS_FAULTADDR_HI),
	    dfm_r(dfm_gpu, AS0 + AS_FAULTADDR_LO));
	if (*result == 0xCAFEBABE12345678ULL) {
		extern void df_mali_frag(void);
		df_mali_frag();
	}
}

/* ---------------- Phase 2b: fragment job clears the whole 1080x2400 framebuffer ---------------- */
/* Descriptor layouts: Mesa src/panfrost/lib/genxml/v7.xml (Bifrost v7 = Mali-G52). */
#define JS0               (0x1800 + 0 * 0x80)
#define DFM_FB_PA         0x7ce6a000ULL
#define DFM_FB_W          1080
#define DFM_FB_H          2400
#define DFM_SW_W          1080
#define DFM_SW_H          2400
#define DFM_SW_STRIDE     (1088 * 4)       /* 4352 B = 68 * 64 B, natively 64B-aligned */

static uint64_t dfm_l2_gb[3];
static int dfm_l2_n;

static void
dfm_map_gb2(uint64_t base_pa, uint64_t pa_any)
{
	uint64_t gb = pa_any >> 30;
	for (int i = 0; i < dfm_l2_n; i++) {
		if (dfm_l2_gb[i] == gb) {
			return;
		}
	}
	if (dfm_l2_n >= 3) {
		printf("mali: out of L2 tables for GB %llu\n", gb);
		return;
	}
	int n = dfm_l2_n++;
	dfm_l2_gb[n] = gb;
	dfm_map_gb((uint64_t *)dfm_mem, (uint64_t *)(dfm_mem + 0x1000 * (n + 1)),
	    base_pa + 0x1000 * (uint64_t)(n + 1), pa_any);
}

void df_mali_frag(void);
void
df_mali_frag(void)
{
	uint64_t base = ml_vtophys((vm_offset_t)dfm_mem);
	uint64_t heap = ml_vtophys((vm_offset_t)dfm_heap);
	printf("mali: frag: mem pa 0x%llx heap pa 0x%llx fb pa 0x%llx\n", base, heap, DFM_FB_PA);
	for (unsigned i = 0; i < sizeof(dfm_mem); i++) {
		dfm_mem[i] = 0;
	}
	dfm_l2_n = 0;
	dfm_map_gb2(base, base);
	dfm_map_gb2(base, heap);
	dfm_map_gb2(base, DFM_FB_PA);

	uint32_t *job = (uint32_t *)(dfm_mem + 0x4000);   /* fragment job: header 32 B + payload */
	uint32_t *fbd = (uint32_t *)(dfm_mem + 0x4100);   /* params 64 B + padding 64 B + RT 64 B */
	uint32_t *rt = fbd + 32;
	uint32_t *tctx = (uint32_t *)(dfm_mem + 0x4400); /* tiler context, 192 B */
	uint32_t *theap = (uint32_t *)(dfm_mem + 0x4600); /* tiler heap descriptor, 32 B */
	uint16_t *sloc = (uint16_t *)(dfm_mem + 0x4700); /* sample locations */
	uint64_t fbd_pa = base + 0x4100, tctx_pa = base + 0x4400, theap_pa = base + 0x4600;
	uint64_t sloc_pa = base + 0x4700;

	for (int i = 0; i < 64; i++) {
		sloc[i] = 128;                      /* every sample at the pixel centre */
	}

	/* Tiler heap + context (empty polygon list: no draws, the fragment job only clears) */
	theap[1] = sizeof(dfm_heap);
	theap[2] = (uint32_t)heap;
	theap[3] = (uint32_t)(heap >> 32);
	theap[4] = (uint32_t)heap;
	theap[5] = (uint32_t)(heap >> 32);
	theap[6] = (uint32_t)(heap + sizeof(dfm_heap));
	theap[7] = (uint32_t)((heap + sizeof(dfm_heap)) >> 32);
	tctx[2] = 0x28;                         /* hierarchy mask */
	tctx[3] = (DFM_FB_W - 1) | ((uint32_t)(DFM_FB_H - 1) << 16);
	tctx[6] = (uint32_t)theap_pa;
	tctx[7] = (uint32_t)(theap_pa >> 32);

	/* Framebuffer Parameters */
	fbd[4] = (uint32_t)sloc_pa;
	fbd[5] = (uint32_t)(sloc_pa >> 32);
	fbd[8] = (DFM_FB_W - 1) | ((uint32_t)(DFM_FB_H - 1) << 16);
	fbd[9] = 0;                                                   /* bound min x/y */
	fbd[10] = (DFM_FB_W - 1) | ((uint32_t)(DFM_FB_H - 1) << 16);  /* bound max x/y */
	fbd[11] = (0u << 0)  /* 1 sample */ | (8u << 9) /* tile 256 px = 16x16 */ |
	    (0u << 19) /* 1 RT */ | (1u << 24) /* 1 KB colour buffer per tile */;
	fbd[12] = (1u << 29);   /* Empty Tile Write Enable: no draws, every tile is "empty" */
	fbd[14] = (uint32_t)tctx_pa;
	fbd[15] = (uint32_t)(tctx_pa >> 32);

	/* Render Target 0: RGBA8 tile buffer -> linear BGRA writeback straight into the LCD buffer */
	rt[0] = (1u << 26);                                           /* internal format R8G8B8A8 */
	rt[1] = (1u << 0) /* write enable */ | (19u << 3) /* R8G8B8A8 */ | (2u << 8) /* linear */ |
	    ((2u | (1u << 3) | (0u << 6) | (3u << 9)) << 16) |        /* swizzle B,G,R,A */
	    (1u << 31);                                               /* clean pixel write enable */
	rt[8] = (uint32_t)DFM_FB_PA;
	rt[9] = (uint32_t)(DFM_FB_PA >> 32);
	rt[10] = DFM_FB_W * 4;                                         /* row stride */
	rt[11] = DFM_FB_W * DFM_FB_H * 4;                              /* surface stride */
	rt[12] = 0x20u | (0x80u << 8) | (0xFFu << 16) | (0xFFu << 24); /* clear R,G,B,A */

	/* Fragment job */
	job[4] = 1u | (9u << 1) | (1u << 16);                          /* 64b, FRAGMENT, index 1 */
	job[8] = 0;                                                   /* bound min tile x/y */
	job[9] = ((DFM_FB_W - 1) >> 4) | (((uint32_t)(DFM_FB_H - 1) >> 4) << 16);
	uint64_t fbptr = fbd_pa | 1u;                                  /* MFBD, 1 RT, no ZS ext */
	job[10] = (uint32_t)fbptr;
	job[11] = (uint32_t)(fbptr >> 32);
	dfm_clean(dfm_mem, sizeof(dfm_mem));

	dfm_as_cmd(1 /* UPDATE: reload AS0 config, drop stale TLB entries */);
	dfm_w(dfm_gpu, MMU_IRQ_CLEAR, 0xffffffffu);
	dfm_w(dfm_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	uint64_t job_pa = base + 0x4000;
	uint64_t t0 = mach_absolute_time();
	dfm_w(dfm_gpu, JS0 + JS_HEAD_NEXT_LO, (uint32_t)job_pa);
	dfm_w(dfm_gpu, JS0 + JS_HEAD_NEXT_HI, (uint32_t)(job_pa >> 32));
	dfm_w(dfm_gpu, JS0 + JS_AFFINITY_NEXT_LO, 0x3);
	dfm_w(dfm_gpu, JS0 + JS_AFFINITY_NEXT_HI, 0);
	dfm_w(dfm_gpu, JS0 + JS_CONFIG_NEXT, 0u | (3u << 8) | (3u << 12) | (8u << 16));
	dfm_w(dfm_gpu, JS0 + JS_COMMAND_NEXT, 1);
	uint32_t raw = 0;
	for (int i = 0; i < 50000; i++) {                             /* ~500 ms: JS0 DONE / FAIL */
		raw = dfm_r(dfm_gpu, JOB_IRQ_RAWSTAT);
		if (raw & ((1u << 0) | (1u << 16))) {
			break;
		}
		dfm_udelay(10);
	}
	uint64_t t1 = mach_absolute_time();
	dfm_clean(job, 64);
	volatile uint32_t *fbv = (volatile uint32_t *)ml_io_map(DFM_FB_PA, 0x1000);
	printf("mali: frag raw 0x%x js0 status 0x%x exc 0x%x fault 0x%x%08x | %llu ticks (%llu us) | fb[0]=0x%x\n",
	    raw, dfm_r(dfm_gpu, JS0 + JS_STATUS), job[0], job[3], job[2], t1 - t0, (t1 - t0) / 13,
	    fbv ? fbv[0] : 0);
	printf("mali: frag mmu raw 0x%x fault 0x%x addr 0x%x%08x\n", dfm_r(dfm_gpu, MMU_IRQ_RAWSTAT),
	    dfm_r(dfm_gpu, AS0 + AS_FAULTSTATUS), dfm_r(dfm_gpu, AS0 + AS_FAULTADDR_HI),
	    dfm_r(dfm_gpu, AS0 + AS_FAULTADDR_LO));
	if ((raw & 1u) && job[0] == 1) {
		extern void df_mali_comp_arm(void);
		df_mali_comp_arm();             /* SwapEnd now composites on the GPU */
	}
}

/* ---------------- Phase 3: GPU compositor for SwapEnd (pre-frame shader from Mesa) ---------------- */
/*
 * df_mali_blob.h (tools/mali_gen, Mesa panfrost v7) holds a pre-frame Draw + RSD + blend + 2 textures +
 * sampler + compiled fragment shader, built for fixed GPU VAs: blob @ DF_MALI_BLOB_VA, iOS SW frame @
 * DF_MALI_SWFB_VA, wallpaper @ DF_MALI_WALL_VA. Here: 4K page tables for those VAs (+ 2 MB identity
 * blocks for the LCD buffer, job memory and tiler heap), then one fragment job per SwapEnd whose
 * framebuffer runs the shader on every pixel (Pre Frame 0 = Always) and writes the LCD directly.
 */
extern const uint32_t df_wall_px[];
static uint8_t dfm_pt[0x14000] __attribute__((aligned(0x4000)));   /* L1 + L2s + L3s (80 KB) */
static unsigned dfm_pt_used;
static uint64_t dfm_pt_pa;
static int dfm_comp_state;          /* 0 off, 1 armed (after Ctrl-P), 2 tables built, -1 failed */
static const uint32_t *dfm_comp_swfb;
static uint32_t dfm_comp_frames;
static int dfm_comp_solid;          /* bisect variant: 0 main, 1 fc, 2 tex64, 3 solid */
static uint32_t dfm_var_frames;
static const char *const dfm_var_name[4] = { "main", "fc (frag coord only)", "tex64 (64B strides)", "solid magenta" };

static uint64_t *
dfm_pt_alloc(uint64_t *pa)
{
	if (dfm_pt_used + 0x1000 > sizeof(dfm_pt)) {
		return 0;
	}
	uint64_t *t = (uint64_t *)(dfm_pt + dfm_pt_used);
	*pa = dfm_pt_pa + dfm_pt_used;
	dfm_pt_used += 0x1000;
	return t;
}

/* next-level table for entry e of table t (creating it if needed) */
static uint64_t *
dfm_pt_next(uint64_t *t, unsigned e)
{
	if ((t[e] & 3) == 3) {
		uint64_t pa = t[e] & 0x0000fffffffff000ULL;
		return (uint64_t *)(dfm_pt + (pa - dfm_pt_pa));
	}
	uint64_t pa;
	uint64_t *n = dfm_pt_alloc(&pa);
	if (n) {
		t[e] = pa | 3;
	}
	return n;
}

#define DFM_PTE_ATTR ((1ULL << 6) | (1ULL << 10) | (1ULL << 11))   /* EL0 RW, AF, nG, AttrIdx 0 */

static int
dfm_map4k(uint64_t va, uint64_t pa)
{
	uint64_t *l1 = (uint64_t *)dfm_pt;
	uint64_t *l2 = dfm_pt_next(l1, (unsigned)(va >> 30));
	uint64_t *l3 = l2 ? dfm_pt_next(l2, (unsigned)((va >> 21) & 511)) : 0;
	if (!l3) {
		return -1;
	}
	l3[(va >> 12) & 511] = (pa & ~0xfffULL) | 3 | DFM_PTE_ATTR;
	return 0;
}

static int
dfm_map_block_gb(uint64_t pa_any)
{
	uint64_t *l1 = (uint64_t *)dfm_pt;
	uint64_t *l2 = dfm_pt_next(l1, (unsigned)(pa_any >> 30));
	if (!l2) {
		return -1;
	}
	uint64_t gb = pa_any >> 30;
	for (uint64_t i = 0; i < 512; i++) {
		if ((l2[i] & 3) != 3) {
			l2[i] = (gb << 30) | (i << 21) | 1 | DFM_PTE_ATTR;
		}
	}
	return 0;
}

static int
dfm_map_kva(uint64_t va, const void *kva, uint64_t len)
{
	for (uint64_t off = 0; off < len; off += 0x1000) {
		uint64_t pa = ml_vtophys((vm_offset_t)kva + off);
		if (!pa || dfm_map4k(va + off, pa)) {
			return -1;
		}
	}
	return 0;
}

static int
dfm_comp_build(const uint32_t *swfb)
{
	dfm_pt_pa = ml_vtophys((vm_offset_t)dfm_pt);
	for (unsigned i = 0; i < sizeof(dfm_pt); i++) {
		dfm_pt[i] = 0;
	}
	dfm_pt_used = 0x1000;                                  /* L1 lives at offset 0 */
	uint8_t *blobk = dfm_mem + 0x6000;                     /* 4K-aligned copy of the blob */
	for (unsigned i = 0; i < 0x2000; i++) {
		blobk[i] = 0;
	}
	{
		const uint8_t *src = df_mali_blob;
		unsigned n = DF_MALI_BLOB_SIZE;
		if (dfm_comp_solid == 1) { src = df_mali_blob_fc; n = DF_MALI_FC_SIZE; }
		if (dfm_comp_solid == 2) { src = df_mali_blob_tex64; n = DF_MALI_TEX64_SIZE; }
		if (dfm_comp_solid == 3) { src = df_mali_blob_solid; n = DF_MALI_SOLID_SIZE; }
		for (unsigned i = 0; i < n; i++) {
			blobk[i] = src[i];
		}
		static unsigned s_build_logs = 0;
		if (s_build_logs < 5) {
			printf("mali: comp variant %d: %s\n", dfm_comp_solid, dfm_var_name[dfm_comp_solid]);
		}
		s_build_logs++;
	}
	int e = 0;
	e |= dfm_map_kva(DF_MALI_BLOB_VA, blobk, 0x2000);
	e |= dfm_map_kva(DF_MALI_SWFB_VA, swfb, (uint64_t)DFM_SW_STRIDE * DFM_SW_H); /* native 1080x2400 @ 4352 B */
	e |= dfm_map_kva(DF_MALI_WALL_VA, df_wall_px, 544ULL * 1200 * 4);
	e |= dfm_map_block_gb(DFM_FB_PA);
	e |= dfm_map_block_gb(ml_vtophys((vm_offset_t)dfm_mem));
	e |= dfm_map_block_gb(ml_vtophys((vm_offset_t)dfm_heap));
	{
		static unsigned s_tbl_logs = 0;
		if (e || s_tbl_logs++ < 5) {
			printf("mali: comp tables %s, %u KB used, pt pa 0x%llx, regs %u\n", e ? "FAILED" : "ok",
			    dfm_pt_used >> 10, dfm_pt_pa, DF_MALI_WORK_REGS);
		}
	}
	if (e) {
		return -1;
	}
	dfm_clean(dfm_pt, dfm_pt_used);
	dfm_clean(dfm_mem, sizeof(dfm_mem));
	dfm_w(dfm_gpu, AS0 + AS_TRANSTAB_LO, (uint32_t)dfm_pt_pa);
	dfm_w(dfm_gpu, AS0 + AS_TRANSTAB_HI, (uint32_t)(dfm_pt_pa >> 32));
	if (dfm_as_cmd(1 /* UPDATE */)) {
		return -1;
	}
	return dfm_as_cmd(4 /* FLUSH_PT */);
}

/* Called from SwapEnd (df_mfb.cpp). 0 = GPU drew the frame, -1 = use the CPU path. */
int df_mali_compose(const uint32_t *swfb);
int
df_mali_compose(const uint32_t *swfb)
{
	if (dfm_comp_state <= 0 || !swfb || !dfm_gpu) {
		return -1;
	}
	if (dfm_comp_state == 1 || swfb != dfm_comp_swfb) {
		dfm_comp_swfb = swfb;
		if (dfm_comp_build(swfb)) {
			dfm_comp_state = -1;
			return -1;
		}
		dfm_comp_state = 2;
	}
	uint64_t base = ml_vtophys((vm_offset_t)dfm_mem);
	uint64_t heap = ml_vtophys((vm_offset_t)dfm_heap);
	uint32_t *job = (uint32_t *)(dfm_mem + 0x5000);
	uint32_t *fbd = (uint32_t *)(dfm_mem + 0x5100);
	uint32_t *rt = fbd + 32;
	uint32_t *tctx = (uint32_t *)(dfm_mem + 0x5400);
	uint32_t *theap = (uint32_t *)(dfm_mem + 0x5600);
	uint16_t *sloc = (uint16_t *)(dfm_mem + 0x5700);
	for (unsigned i = 0x5000; i < 0x5800; i++) {
		dfm_mem[i] = 0;
	}
	for (int i = 0; i < 64; i++) {
		sloc[i] = 128;
	}
	uint64_t tctx_pa = base + 0x5400, theap_pa = base + 0x5600, sloc_pa = base + 0x5700;
	theap[1] = 0x40000;
	theap[2] = (uint32_t)heap;  theap[3] = (uint32_t)(heap >> 32);
	theap[4] = (uint32_t)heap;  theap[5] = (uint32_t)(heap >> 32);
	theap[6] = (uint32_t)(heap + 0x40000);  theap[7] = (uint32_t)((heap + 0x40000) >> 32);
	tctx[2] = 0x28;
	tctx[3] = (DFM_FB_W - 1) | ((uint32_t)(DFM_FB_H - 1) << 16);
	tctx[6] = (uint32_t)theap_pa;  tctx[7] = (uint32_t)(theap_pa >> 32);

	fbd[0] = 1;                                         /* Pre Frame 0 = Always */
	fbd[4] = (uint32_t)sloc_pa;  fbd[5] = (uint32_t)(sloc_pa >> 32);
	fbd[6] = (uint32_t)DF_MALI_DCD_VA;  fbd[7] = (uint32_t)(DF_MALI_DCD_VA >> 32);
	fbd[8] = (DFM_FB_W - 1) | ((uint32_t)(DFM_FB_H - 1) << 16);
	fbd[10] = (DFM_FB_W - 1) | ((uint32_t)(DFM_FB_H - 1) << 16);
	fbd[11] = (8u << 9) | (1u << 24);                   /* 16x16 tiles, 1 RT, 1 KB/tile */
	fbd[12] = (1u << 29);                               /* write every tile */
	fbd[14] = (uint32_t)tctx_pa;  fbd[15] = (uint32_t)(tctx_pa >> 32);
	rt[0] = (1u << 26);
	rt[1] = 1u | (19u << 3) | (2u << 8) | ((2u | (1u << 3) | (0u << 6) | (3u << 9)) << 16) | (1u << 31);
	rt[8] = (uint32_t)DFM_FB_PA;  rt[9] = (uint32_t)(DFM_FB_PA >> 32);
	rt[10] = DFM_FB_W * 4;
	rt[11] = DFM_FB_W * DFM_FB_H * 4;
	job[4] = 1u | (9u << 1) | (1u << 16);
	job[9] = ((DFM_FB_W - 1) >> 4) | (((uint32_t)(DFM_FB_H - 1) >> 4) << 16);
	uint64_t fbptr = (base + 0x5100) | 1u;
	job[10] = (uint32_t)fbptr;  job[11] = (uint32_t)(fbptr >> 32);

	uint64_t t0 = mach_absolute_time();
	/* the CPU rendered 720x1600 into cacheable memory; the GPU reads it non-cacheable */
	for (uintptr_t a = (uintptr_t)swfb & ~63UL; a < (uintptr_t)swfb + (uintptr_t)DFM_SW_STRIDE * DFM_SW_H; a += 64) {
		__asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
	}
	dfm_clean(dfm_mem + 0x5000, 0x800);
	uint64_t t1 = mach_absolute_time();
	dfm_w(dfm_gpu, JOB_IRQ_CLEAR, 0xffffffffu);
	uint64_t job_pa = base + 0x5000;
	dfm_w(dfm_gpu, JS0 + JS_HEAD_NEXT_LO, (uint32_t)job_pa);
	dfm_w(dfm_gpu, JS0 + JS_HEAD_NEXT_HI, (uint32_t)(job_pa >> 32));
	dfm_w(dfm_gpu, JS0 + JS_AFFINITY_NEXT_LO, 0x3);
	dfm_w(dfm_gpu, JS0 + JS_AFFINITY_NEXT_HI, 0);
	dfm_w(dfm_gpu, JS0 + JS_CONFIG_NEXT, 0u | (3u << 8) | (3u << 12) | (8u << 16));
	dfm_w(dfm_gpu, JS0 + JS_COMMAND_NEXT, 1);
	uint32_t raw = 0;
	for (int i = 0; i < 20000; i++) {                   /* <= ~200 ms */
		raw = dfm_r(dfm_gpu, JOB_IRQ_RAWSTAT);
		if (raw & ((1u << 0) | (1u << 16))) {
			break;
		}
		dfm_udelay(10);
	}
	uint64_t t2 = mach_absolute_time();
	dfm_clean(job, 64);
	int ok = (raw & 1u) && !(raw & (1u << 16)) && job[0] == 1;
	if (!ok || dfm_comp_frames < 5 || (dfm_comp_frames % 300) == 0) {
		printf("mali: comp #%u %s raw 0x%x exc 0x%x gfault 0x%x @0x%x | flush %llu us gpu %llu us | mmu 0x%x fault 0x%x @0x%x\n",
		    dfm_comp_frames, ok ? "ok" : "FAIL", raw, job[0], dfm_r(dfm_gpu, 0x3C), dfm_r(dfm_gpu, 0x40),
		    (t1 - t0) / 13, (t2 - t1) / 13,
		    dfm_r(dfm_gpu, MMU_IRQ_RAWSTAT), dfm_r(dfm_gpu, AS0 + AS_FAULTSTATUS),
		    dfm_r(dfm_gpu, AS0 + AS_FAULTADDR_LO));
	}
	dfm_comp_frames++;

	if (0 && dfm_comp_solid < 3 && (!ok || (dfm_comp_solid > 0 && ++dfm_var_frames >= 2))) {   /* bisect done: main only */
		/* bisect: try the next shader variant on the same pipeline */
		dfm_comp_solid++;
		dfm_var_frames = 0;
		dfm_comp_state = 1;
		return ok ? 0 : df_mali_compose(swfb);
	}
	if (dfm_comp_solid == 3 && ok && ++dfm_var_frames >= 2) {
		printf("mali: bisect done, back to the CPU compositor\n");
		dfm_comp_state = -1;
		return 0;
	}
	if (!ok) {
		dfm_comp_state = -1;                            /* fall back to the CPU for good */
		return -1;
	}
	return 0;
}

void df_mali_comp_arm(void);
void
df_mali_comp_arm(void)
{
	dfm_comp_state = 1;
	dfm_comp_frames = 0;
	printf("mali: GPU compositor armed (next SwapEnd draws on the Mali)\n");
}

int df_mali_is_armed(void);
int
df_mali_is_armed(void)
{
	return dfm_comp_state > 0;
}

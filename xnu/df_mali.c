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
static uint8_t dfm_mem[0x4000] __attribute__((aligned(0x4000)));

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
}

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
}

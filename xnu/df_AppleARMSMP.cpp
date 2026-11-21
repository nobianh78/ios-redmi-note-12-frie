/*
 * DarwinFire: AppleARMSMP replacement for the Redmi 12 "fire" (MT6768).
 * Based on xnu-8796.141.3 iokit/Kernel/arm/AppleARMSMP.cpp (APSL 2.0, Apple Inc.).
 *
 * Apple silicon brings secondary cores up through IOPMGR (PMGR power controller) and delivers
 * IPIs through AIC / Apple fast-IPI sysregs. Neither exists on MediaTek, so here:
 *   - secondary cores are released with PSCI CPU_ON (SMC to the MediaTek ATF bl31), entry point =
 *     LowResetVectorBase (the same reset path a VMAPPLE guest uses);
 *   - IPIs are GICv3 SGIs (ICC_SGI1R_EL1, INTID DF_IPI_SGI); sleh_irq hands them to df_smp_ipi().
 * No CPU idle power management and no cluster power-down: idle cores just WFI.
 * Copied over iokit/Kernel/arm/AppleARMSMP.cpp by xnu/fix-xcode16.sh.
 */

extern "C" {
#include <kern/debug.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>
};

#include <kern/bits.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <kperf/kperf.h>
#include <machine/machine_routines.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSCollection.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformActions.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <Kernel/IOKitKernelInternal.h>

#if !USE_APPLEARMSMP
#error "df_AppleARMSMP.cpp expects USE_APPLEARMSMP"
#endif

extern "C" void console_suspend();
extern "C" void console_resume();
extern "C" void df_gicr_enable_sgi(void);   /* pexpert/arm/pe_fiq.c (fix-xcode16) */
extern "C" void df_smp_ipi(void);

#define DF_IPI_SGI       1u
#define PSCI_CPU_ON_64   0xC4000003ULL

static const ml_topology_info *topology_info;
static unsigned int boot_cpu;
static processor_t *machProcessors;
static ipi_handler_t df_ipi_handler;
static unsigned int df_max_cpus = 2;   /* raise to 6 once 2-core boot is stable */

bool cluster_power_supported = false;
static uint64_t cpu_power_state_mask;
static uint64_t all_clusters_mask;
static uint64_t online_clusters_mask;

static unsigned int
target_to_cpu_id(cpu_id_t in)
{
	return (unsigned int)(uintptr_t)in;
}

static uint64_t
df_psci_cpu_on(uint64_t mpidr, uint64_t entry)
{
	register uint64_t x0 asm("x0") = PSCI_CPU_ON_64;
	register uint64_t x1 asm("x1") = mpidr;
	register uint64_t x2 asm("x2") = entry;
	register uint64_t x3 asm("x3") = 0;
	__asm__ volatile ("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
	    :
	    : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
	    "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * Clean every data/unified cache level to the point of coherency by set/way.
 * flush_dcache() is only a DSB on this XNU, but a core released by PSCI runs the reset vector
 * with MMU and caches off and must see ResetHandlerData / CpuDataEntries / cpu_data in DRAM.
 */
static void
df_clean_dcache_all(void)
{
	uint64_t clidr, ccsidr;
	__asm__ volatile ("dsb sy" ::: "memory");
	__asm__ volatile ("mrs %0, CLIDR_EL1" : "=r"(clidr));
	unsigned loc = (clidr >> 24) & 7;
	for (unsigned level = 0; level < loc; level++) {
		unsigned ctype = (clidr >> (level * 3)) & 7;
		if (ctype < 2) {
			continue;       /* no cache or I-cache only */
		}
		__asm__ volatile ("msr CSSELR_EL1, %0; isb" :: "r"((uint64_t)(level << 1)));
		__asm__ volatile ("mrs %0, CCSIDR_EL1" : "=r"(ccsidr));
		unsigned line_shift = (unsigned)(ccsidr & 7) + 4;
		unsigned ways = (unsigned)((ccsidr >> 3) & 0x3ff);
		unsigned sets = (unsigned)((ccsidr >> 13) & 0x7fff);
		unsigned way_shift = ways ? (unsigned)__builtin_clz(ways) : 0;
		for (unsigned w = 0; w <= ways; w++) {
			for (unsigned s = 0; s <= sets; s++) {
				uint64_t v = ((uint64_t)w << way_shift) | ((uint64_t)s << line_shift) | (level << 1);
				__asm__ volatile ("dc csw, %0" :: "r"(v) : "memory");
			}
		}
	}
	__asm__ volatile ("msr CSSELR_EL1, xzr; dsb sy; isb" ::: "memory");
}

/* GICv3 SGI to one core. fire: every core has aff0 = 0, aff1 = core number. */
static void
df_send_sgi(uint32_t phys_id)
{
	uint64_t aff1 = (phys_id >> 8) & 0xff;
	uint64_t aff0 = phys_id & 0xf;
	uint64_t v = ((uint64_t)DF_IPI_SGI << 24) | (aff1 << 16) | (1ULL << aff0);
	__asm__ volatile ("dsb sy" ::: "memory");
	__builtin_arm_wsr64("ICC_SGI1R_EL1", v);
	__asm__ volatile ("isb" ::: "memory");
}

/* Called from sleh_irq (fix-xcode16) for SGI DF_IPI_SGI on the receiving core. */
extern "C" void
df_smp_ipi(void)
{
	if (df_ipi_handler) {
		df_ipi_handler();
	}
}

static void
cpu_boot_thread(void */*unused0*/, wait_result_t /*unused1*/)
{
	OSDictionary *matching = IOService::serviceMatching("IOPlatformExpert");
	IOService::waitForMatchingService(matching, UINT64_MAX);
	matching->release();

	PE_parse_boot_argn("df_cpus", &df_max_cpus, sizeof(df_max_cpus));
	unsigned int ncpu = topology_info->num_cpus;
	if (ncpu > df_max_cpus) {
		ncpu = df_max_cpus;
	}
	if (ncpu < 1) {
		ncpu = 1;
	}
	printf("df_smp: %u CPUs in topology, bringing up %u (boot cpu %u)\n",
	    topology_info->num_cpus, ncpu, boot_cpu);

	ml_set_max_cpus(topology_info->max_cpu_id + 1);

	const size_t array_size = (topology_info->max_cpu_id + 1) * sizeof(*machProcessors);
	machProcessors = static_cast<processor_t *>(zalloc_permanent(array_size, ZALIGN_PTR));

	for (unsigned int cpu = 0; cpu < topology_info->num_cpus; cpu++) {
		const ml_topology_cpu *cpu_info = &topology_info->cpus[cpu];
		const unsigned int cpu_id = cpu_info->cpu_id;
		ml_processor_info_t this_processor_info;
		ipi_handler_t ipi_handler = NULL;
		perfmon_interrupt_handler_func pmi_handler = NULL;

		memset(&this_processor_info, 0, sizeof(this_processor_info));
		this_processor_info.cpu_id = reinterpret_cast<cpu_id_t>(cpu_id);
		this_processor_info.phys_id = cpu_info->phys_id;
		this_processor_info.log_id = cpu_id;
		this_processor_info.cluster_id = cpu_info->cluster_id;
		this_processor_info.cluster_type = cpu_info->cluster_type;
		this_processor_info.l2_cache_size = cpu_info->l2_cache_size;
		this_processor_info.l2_cache_id = cpu_info->l2_cache_id;
		this_processor_info.l3_cache_size = cpu_info->l3_cache_size;
		this_processor_info.l3_cache_id = cpu_info->l3_cache_id;

		kern_return_t result = ml_processor_register(&this_processor_info,
		    &machProcessors[cpu_id], &ipi_handler, &pmi_handler);
		if (result == KERN_FAILURE) {
			panic("ml_processor_register failed: %d", result);
		}
		if (ipi_handler) {
			df_ipi_handler = ipi_handler;
		}
	}

	for (unsigned int cpu = 0; cpu < topology_info->num_cpus; cpu++) {
		const unsigned int cpu_id = topology_info->cpus[cpu].cpu_id;
		if (cpu_id != boot_cpu && cpu >= ncpu) {
			continue;
		}
		if (processor_start(machProcessors[cpu_id]) != KERN_SUCCESS) {
			panic("processor_start failed");
		}
	}
	ml_cpu_init_completed();
	IOService::publishResource(gIOAllCPUInitializedKey, kOSBooleanTrue);
	printf("df_smp: cpu init completed\n");
}

void
IOCPUInitialize(void)
{
	topology_info = ml_get_topology_info();
	boot_cpu = topology_info->boot_cpu->cpu_id;

	for (unsigned int i = 0; i < topology_info->num_clusters; i++) {
		bit_set(all_clusters_mask, topology_info->clusters[i].cluster_id);
	}
	online_clusters_mask = all_clusters_mask;

	thread_t thread;
	kernel_thread_start(&cpu_boot_thread, NULL, &thread);
	thread_set_thread_name(thread, "cpu_boot_thread");
	thread_deallocate(thread);
}

// Release a secondary CPU from reset via PSCI CPU_ON. Runs from a different CPU.
kern_return_t
PE_cpu_start(cpu_id_t target,
    vm_offset_t /*start_paddr*/, vm_offset_t /*arg_paddr*/)
{
	unsigned int cpu_id = target_to_cpu_id(target);

	if (cpu_id != boot_cpu) {
		extern unsigned int LowResetVectorBase;
		uint64_t entry = ml_vtophys((vm_offset_t)&LowResetVectorBase);
		uint64_t mpidr = topology_info->cpus[cpu_id].phys_id;
		boolean_t istate = ml_set_interrupts_enabled(FALSE);
		df_clean_dcache_all();
		uint64_t r = df_psci_cpu_on(mpidr, entry);
		ml_set_interrupts_enabled(istate);
		printf("df_smp: PSCI CPU_ON cpu %u mpidr 0x%llx entry 0x%llx -> %lld\n",
		    cpu_id, mpidr, entry, (long long)r);
		if ((int64_t)r != 0 && (int64_t)r != -4 /* ALREADY_ON */) {
			return KERN_FAILURE;
		}
	}
	return KERN_SUCCESS;
}

// Initialize a CPU when it first comes up. Runs on the target CPU.
void
PE_cpu_machine_init(cpu_id_t target, boolean_t bootb)
{
	unsigned int cpu_id = target_to_cpu_id(target);

	if (!bootb && cpu_id == boot_cpu && ml_is_quiescing()) {
		IOCPURunPlatformActiveActions();
	}

	df_gicr_enable_sgi();
	ml_broadcast_cpu_event(CPU_BOOTED, cpu_id);

	// Send myself an IPI to clear SIGPdisabled. Hang here if IPIs are broken.
	PE_cpu_signal(target, target);
	uint64_t spins = 0;
	while (ml_get_interrupts_enabled() && !ml_cpu_signal_is_enabled()) {
		OSMemoryBarrier();
		if (++spins == 50000000ULL) {
			printf("df_smp: cpu %u still waiting for self-IPI\n", cpu_id);
		}
	}
	if (cpu_id != boot_cpu) {
		printf("df_smp: cpu %u online\n", cpu_id);
	}
}

void
PE_cpu_halt(cpu_id_t target)
{
	unsigned int cpu_id = target_to_cpu_id(target);
	processor_exit(machProcessors[cpu_id]);
}

void
PE_cpu_signal(cpu_id_t /*source*/, cpu_id_t target)
{
	struct ml_topology_cpu *cpu = &topology_info->cpus[target_to_cpu_id(target)];
	df_send_sgi(cpu->phys_id);
}

void
PE_cpu_signal_deferred(cpu_id_t source, cpu_id_t target)
{
	PE_cpu_signal(source, target);
}

void
PE_cpu_signal_cancel(cpu_id_t /*source*/, cpu_id_t /*target*/)
{
}

void
PE_cpu_machine_quiesce(cpu_id_t target)
{
	unsigned int cpu_id = target_to_cpu_id(target);

	if (cpu_id == boot_cpu) {
		IOCPURunPlatformQuiesceActions();
	}
	ml_broadcast_cpu_event(CPU_DOWN, cpu_id);
	ml_arm_sleep();
}

bool
PE_cpu_down(cpu_id_t target)
{
	unsigned int cpu_id = target_to_cpu_id(target);
	ml_broadcast_cpu_event(CPU_DOWN, cpu_id);
	return false;
}

void
PE_handle_ext_interrupt(void)
{
	/* fire: device IRQs are handled/acked directly in sleh_irq (fix-xcode16) */
}

void
PE_cpu_power_disable(int cpu_id)
{
	bit_clear(cpu_power_state_mask, cpu_id);
}

void
PE_cpu_power_enable(int cpu_id)
{
	bit_set(cpu_power_state_mask, cpu_id);
}

void
IOCPUSleepKernel(void)
{
	/* fire has no S2R support; never called (no IOPMrootDomain sleep). */
	printf("IOCPUSleepKernel: not supported on fire\n");
}

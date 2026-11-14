/*
 * fire_init: the first userland program on the Redmi 12 "fire" XNU port (PID 1, served by mockfs as
 * /sbin/launchd). Static arm64 Mach-O, no libc, no dyld: raw BSD syscalls (svc #0x80, number in x16).
 *
 * It proves userland runs: prints to /dev/console, then exits with status 0x42 on purpose, so the
 * kernel panics with "initproc exited" and that status, which the on-screen panic printer shows.
 * shortcut: no shell yet; this is the stepping stone before building a real Darwin userland.
 */

#define SYS_exit   1
#define SYS_write  4
#define SYS_open   5
#define O_WRONLY   0x0001
#define O_NOCTTY   0x20000

static long
sys3(long num, long a0, long a1, long a2)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	__asm__ volatile ("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory", "cc");
	return x0;
}

static unsigned long
slen(const char *s)
{
	unsigned long n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static void
say(long fd, const char *s)
{
	sys3(SYS_write, fd, (long)s, (long)slen(s));
}

/* LC_UNIXTHREAD entry: the kernel hands over with SP pointing at argc, only 8-byte aligned. AArch64
 * faults on a misaligned SP (SCTLR_EL1.SA0), so align it before any C code touches the stack
 * (the first boot died with an SP alignment fault, ESR EC 0x26, in the spin loop below). */
void cstart(void);
__asm__(".globl _start\n"
    "_start:\n"
    "\tmov x9, sp\n"
    "\tand sp, x9, #~15\n"
    "\tb _cstart\n");

void
cstart(void)
{
	long fd = sys3(SYS_open, (long)"/dev/console", O_WRONLY | O_NOCTTY, 0);

	say(fd, "\n**********************************************\n");
	say(fd, "*  DarwinFire: hello from userland (PID 1)   *\n");
	say(fd, "*  open-source XNU on Redmi 12 fire / MT6768 *\n");
	say(fd, "**********************************************\n\n");
	for (int i = 0; i < 5; i++) {
		say(fd, "fire_init: still alive\n");
		for (volatile unsigned long spin = 0; spin < 200000000ul; spin++) {
		}
	}
	say(fd, "fire_init: exiting with 0x42 on purpose\n");
	sys3(SYS_exit, 0x42, 0, 0);
	for (;;) {
	}
}

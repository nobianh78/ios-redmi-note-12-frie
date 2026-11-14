/*
 * fire_init: PID 1 of the Redmi 12 "fire" XNU port (served by mockfs as /sbin/launchd).
 * Static arm64 Mach-O, no libc, no dyld: raw BSD syscalls (svc #0x80, number in x16).
 *
 * v2: a minimal shell. It runs the built-in script below, echoing each command and its output on
 * /dev/console, then shows a "fire#" prompt and reads commands from the console.
 * shortcut: the script is compiled in because mockfs serves the ramdisk as this single file; read
 * scripts from a real root filesystem once there is one. No input driver yet, so the prompt only
 * proves the read path; typing needs a touchscreen/keys driver.
 */

#define SYS_exit            1
#define SYS_read            3
#define SYS_write           4
#define SYS_open            5
#define SYS_close           6
#define SYS_getpid          20
#define SYS_getppid         39
#define SYS_select          93
#define SYS_gettimeofday    116
#define SYS_sysctl          202
#define SYS_getdirentries64 344
#define SYS_fsync           95
#define SYS_mkdir           136
#define SYS_mount           167
#define MNT_UPDATE          0x00010000
#define O_WRONLY    0x0001
#define O_CREAT     0x0200
#define O_TRUNC     0x0400

#define O_RDONLY    0x0000
#define O_RDWR      0x0002
#define O_NOCTTY    0x20000
#define O_DIRECTORY 0x100000

typedef unsigned long size_t;

static long
sys6(long num, long a0, long a1, long a2, long a3, long a4, long a5)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a0;
	register long x1 __asm__("x1") = a1;
	register long x2 __asm__("x2") = a2;
	register long x3 __asm__("x3") = a3;
	register long x4 __asm__("x4") = a4;
	register long x5 __asm__("x5") = a5;
	unsigned long nzcv;
	__asm__ volatile ("svc #0x80\n\tmrs %[f], nzcv"
	    : "+r"(x0), "+r"(x1), [f] "=r"(nzcv)
	    : "r"(x16), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
	    : "memory", "cc");
	return (nzcv & (1ul << 29)) ? -x0 : x0;   /* carry set = error, x0 = errno */
}
#define sys3(n, a, b, c) sys6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

/* -O2 may turn loops into these calls even with -ffreestanding. */
void *
memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--) {
		*p++ = (unsigned char)c;
	}
	return d;
}
void *
memcpy(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	while (n--) {
		*p++ = *q++;
	}
	return d;
}

static long con = -1;

static size_t
slen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}
static int
seq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}
/* Output is line-buffered: the console driver drops 1-byte writes (kmoutput hands them to kmputc,
 * which writes nothing here), so single digits vanished. Flush on newline, when full, and before reads. */
static char obuf[512];
static size_t olen;
static void
flush(void)
{
	if (olen) {
		sys3(SYS_write, con, obuf, olen);
		olen = 0;
	}
}
static void
outn(const char *s, size_t n)
{
	while (n--) {
		if (olen == sizeof(obuf)) {
			flush();
		}
		char c = *s++;
		obuf[olen++] = c;
		if (c == '\n') {
			flush();
		}
	}
}
static void
out(const char *s)
{
	outn(s, slen(s));
}
static void
outu(unsigned long v)
{
	char b[24];
	int i = 24;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	outn(b + i, (size_t)(24 - i));
}
static void
outi(long v)
{
	if (v < 0) {
		out("-");
		v = -v;
	}
	outu((unsigned long)v);
}
static unsigned long
atou(const char *s)
{
	unsigned long v = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (unsigned long)(*s++ - '0');
	}
	return v;
}

/* sysctl by MIB; returns bytes read or a negative errno */
static long
sysctl2(int a, int b, void *buf, size_t len)
{
	int mib[2] = { a, b };
	size_t l = len;
	long r = sys6(SYS_sysctl, (long)mib, 2, (long)buf, (long)&l, 0, 0);
	return r < 0 ? r : (long)l;
}
static void
sysctl_str(int a, int b)
{
	char buf[256];
	long n = sysctl2(a, b, buf, sizeof(buf) - 1);
	if (n <= 0) {
		out("?");
		return;
	}
	buf[n] = 0;
	out(buf);
}

struct tv64 {
	long sec;
	int usec;
	int pad;
};
static long
now_us(void)
{
	struct tv64 tv = { 0, 0, 0 };
	sys3(SYS_gettimeofday, &tv, 0, 0);
	return tv.sec * 1000000L + tv.usec;
}

static void
cmd_uname(const char *arg)
{
	sysctl_str(1, 1);                       /* kern.ostype */
	if (arg && seq(arg, "-a")) {
		out(" ");
		sysctl_str(1, 10);              /* kern.hostname */
		out(" ");
		sysctl_str(1, 2);               /* kern.osrelease */
		out(" ");
		sysctl_str(1, 4);               /* kern.version */
		out(" ");
		sysctl_str(6, 1);               /* hw.machine */
	}
	out("\n");
}
static void
cmd_hw(void)
{
	int ncpu = 0, pg = 0;
	unsigned long mem = 0;
	sysctl2(6, 3, &ncpu, sizeof(ncpu));     /* hw.ncpu */
	sysctl2(6, 7, &pg, sizeof(pg));         /* hw.pagesize */
	sysctl2(6, 24, &mem, sizeof(mem));      /* hw.memsize */
	out("cpus ");
	outi(ncpu);
	out("  pagesize ");
	outi(pg);
	out("  memsize ");
	outu(mem >> 20);
	out(" MB  model ");
	sysctl_str(6, 2);                       /* hw.model */
	out("\n");
}
/* mach_absolute_time Mach trap (-3): the hardware timebase count since boot. */
static unsigned long
abstime(void)
{
	register long x16 __asm__("x16") = -3;
	register unsigned long x0 __asm__("x0");
	__asm__ volatile ("svc #0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
	return x0;
}
static void
cmd_uptime(void)
{
	/* kern.boottime is 0 without an RTC, so count from the timebase instead */
	int tb = 0;
	sysctl2(6, 23, &tb, sizeof(tb));        /* hw.tbfrequency */
	unsigned long t = abstime();
	long up = tb > 0 ? (long)(t / (unsigned long)tb * 1000000UL + t % (unsigned long)tb * 1000000UL / (unsigned long)tb) : 0;
	out("up ");
	outi(up / 1000000L);
	out(".");
	long ms = (up / 1000) % 1000;
	if (ms < 100) {
		out("0");
	}
	if (ms < 10) {
		out("0");
	}
	outi(ms);
	out(" s\n");
}
static void
cmd_sleep(const char *arg)
{
	unsigned long s = arg ? atou(arg) : 1;
	struct tv64 tv = { (long)s, 0, 0 };
	long t0 = now_us();
	long r = sys6(SYS_select, 0, 0, 0, 0, (long)&tv, 0);
	long dt = now_us() - t0;
	out("slept ");
	outi(dt / 1000);
	out(" ms (select ");
	outi(r);
	out(")\n");
}
static void
cmd_ls(const char *path)
{
	long fd = sys3(SYS_open, path ? path : "/", O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) {
		out("ls: open error ");
		outi(-fd);
		out("\n");
		return;
	}
	static char buf[4096];
	long pos = 0, n, count = 0;
	while ((n = sys6(SYS_getdirentries64, fd, (long)buf, sizeof(buf), (long)&pos, 0, 0)) > 0) {
		for (long off = 0; off < n;) {
			unsigned short reclen = *(unsigned short *)(buf + off + 16);
			unsigned short namlen = *(unsigned short *)(buf + off + 18);
			if (reclen == 0) {
				break;
			}
			outn(buf + off + 21, namlen);
			out(++count % 6 ? "  " : "\n");
			off += reclen;
		}
	}
	if (n < 0) {
		out("ls: getdirentries64 error ");
		outi(-n);
	}
	out("\n");
	sys3(SYS_close, fd, 0, 0);
}

static void
cmd_cat(const char *path)
{
	long fd = sys3(SYS_open, path ? path : "", O_RDONLY, 0);
	if (fd < 0) {
		out("cat: open error ");
		outi(-fd);
		out("\n");
		return;
	}
	char buf[512];
	long n;
	while ((n = sys3(SYS_read, fd, buf, sizeof(buf))) > 0) {
		outn(buf, (size_t)n);
	}
	sys3(SYS_close, fd, 0, 0);
}
/* write FILE TEXT: create/truncate FILE and store TEXT plus a newline (tests HFS+ writes). */
static void
cmd_write(char *arg)
{
	char *text = arg;
	while (text && *text && *text != ' ') {
		text++;
	}
	if (!arg || !*text) {
		out("usage: write FILE TEXT\n");
		return;
	}
	*text++ = 0;
	long fd = sys3(SYS_open, arg, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		out("write: open error ");
		outi(-fd);
		out("\n");
		return;
	}
	size_t n = slen(text);
	long w = sys3(SYS_write, fd, text, n);
	sys3(SYS_write, fd, "\n", 1);
	long c = sys3(SYS_fsync, fd, 0, 0);
	sys3(SYS_close, fd, 0, 0);
	out("wrote ");
	outi(w + 1);
	out(" bytes (fsync ");
	outi(c);
	out(")\n");
}

/* ---- threads through libpthread's kernel part (P3): bsdthread_register + bsdthread_create ----
 * The kernel starts each new thread at the registered entry with x0 = pthread, x1 = kernel port,
 * x2 = function, x3 = argument, sp = our stack (PTHREAD_START_CUSTOM: we supply stack and pthread). */
#define SYS_bsdthread_create    360
#define SYS_bsdthread_terminate 361
#define SYS_bsdthread_register  366
#define PTHREAD_START_CUSTOM    0x01000000
void df_thread_entry(void);
__asm__(".globl _df_thread_entry\n"
    "_df_thread_entry:\n"
    "\tmov x19, x1\n"            /* kernel port of this thread */
    "\tmov x0, x3\n"
    "\tblr x2\n"                 /* func(arg) */
    "\tmov x0, #0\n"             /* bsdthread_terminate(stack 0, size 0, port, sem 0) */
    "\tmov x1, #0\n"
    "\tmov x2, x19\n"
    "\tmov x3, #0\n"
    "\tmov x16, #361\n"
    "\tsvc #0x80\n"
    "1:\tb 1b\n");
static volatile int thread_ran;
static void
thread_body(void *arg)
{
	out("  [thread] hello from a second thread, arg=");
	outu((unsigned long)arg);
	out("\n");
	thread_ran = 1;
}
static void
cmd_thread(void)
{
	static int registered;
	static unsigned long stack[4096] __attribute__((aligned(16)));
	static unsigned long pthread_buf[128] __attribute__((aligned(16)));
	if (!registered) {
		long r = sys6(SYS_bsdthread_register, (long)df_thread_entry, (long)df_thread_entry, sizeof(pthread_buf), 0, 0, 0);
		out("bsdthread_register: ");
		outi(r);
		out("\n");
		if (r < 0) {
			return;
		}
		registered = 1;
	}
	thread_ran = 0;
	long r = sys6(SYS_bsdthread_create, (long)thread_body, 42, (long)(stack + 4096), (long)pthread_buf,
	    PTHREAD_START_CUSTOM, 0);
	out("bsdthread_create: ");
	outi(r < 0 ? r : 0);
	out("\n");
	if (r < 0) {
		return;
	}
	struct tv64 tv = { 0, 200000, 0 };
	for (int i = 0; i < 10 && !thread_ran; i++) {
		sys6(SYS_select, 0, 0, 0, 0, (long)&tv, 0);
	}
	out(thread_ran ? "thread: ok, the second thread ran\n" : "thread: the second thread did not run within 2 s\n");
}

/* run PATH [ARGS...]: fork + execve + wait4 (what a real shell needs). argv/envp are built on the
 * stack at run time (no pointers in initialized data: there is no dyld to fix them up). */
#define SYS_fork   2
#define SYS_wait4  7
#define SYS_execve 59
static void
cmd_run(char *arg)
{
	if (!arg) {
		out("usage: run PATH [ARGS...]\n");
		return;
	}
	char *argv[16];
	int argc = 0;
	for (char *p = arg; *p && argc < 15;) {
		argv[argc++] = p;
		while (*p && *p != ' ') {
			p++;
		}
		while (*p == ' ') {
			*p++ = 0;
		}
	}
	argv[argc] = 0;
	char fire_env[] = "FIRE=on-xnu";
	char path_env[] = "PATH=/bin:/sbin";
	char *envp[] = { fire_env, path_env, 0 };
	flush();
	register long x16 __asm__("x16") = SYS_fork;
	register long x0 __asm__("x0") = 0;
	register long x1 __asm__("x1") = 0;
	unsigned long nzcv;
	__asm__ volatile ("svc #0x80\n\tmrs %[v], nzcv" : "+r"(x0), "+r"(x1), [v] "=r"(nzcv) : "r"(x16) : "memory", "cc");
	if (nzcv & (1ul << 29)) {
		out("run: fork error ");
		outi(x0);
		out("\n");
		return;
	}
	if (x1) {               /* child */
		long r = sys3(SYS_execve, argv[0], argv, envp);
		out("run: execve error ");
		outi(-r);
		out("\n");
		flush();
		sys3(SYS_exit, 127, 0, 0);
	}
	long pid = x0;
	int status = 0;
	long w = sys6(SYS_wait4, pid, (long)&status, 0, 0, 0, 0);
	out("run: pid ");
	outi(pid);
	out(" exited, wait4 ");
	outi(w);
	out(" status ");
	outi((status >> 8) & 0xff);
	out("\n");
}

static void
help(void)
{
	out("commands: echo TEXT | uname [-a] | hw | pid | uptime | ls [DIR] | cat FILE | write FILE TEXT |"
	    " mkdir DIR | remount | thread | run PATH [ARGS] | sleep N | help | exit [N]\n");
}

/* Run one command line (modified in place). */
static void
run(char *line)
{
	while (*line == ' ') {
		line++;
	}
	if (!*line) {
		return;
	}
	char *arg = line;
	while (*arg && *arg != ' ') {
		arg++;
	}
	if (*arg) {
		*arg++ = 0;
		while (*arg == ' ') {
			arg++;
		}
	}
	if (!*arg) {
		arg = 0;
	}
	if (seq(line, "echo")) {
		out(arg ? arg : "");
		out("\n");
	} else if (seq(line, "uname")) {
		cmd_uname(arg);
	} else if (seq(line, "hw")) {
		cmd_hw();
	} else if (seq(line, "pid")) {
		out("pid ");
		outi(sys3(SYS_getpid, 0, 0, 0));
		out("  ppid ");
		outi(sys3(SYS_getppid, 0, 0, 0));
		out("\n");
	} else if (seq(line, "uptime")) {
		cmd_uptime();
	} else if (seq(line, "ls")) {
		cmd_ls(arg);
	} else if (seq(line, "cat")) {
		cmd_cat(arg);
	} else if (seq(line, "write")) {
		cmd_write(arg);
	} else if (seq(line, "run")) {
		cmd_run(arg);
	} else if (seq(line, "thread")) {
		cmd_thread();
	} else if (seq(line, "remount")) {
		/* the kernel mounts root read-only (EROFS on write); launchd normally does `mount -uw /` */
		/* HFS copyin()s a struct hfs_mount_args even for an update (NULL gave EFAULT); all-zero
		 * (user layout: fspec ptr, uid, gid, mask, encoding, timezone, flags, journal x3) keeps settings */
		static long hfs_args[8];
		long r = sys6(SYS_mount, (long)"hfs", (long)"/", MNT_UPDATE, (long)hfs_args, 0, 0);
		out(r < 0 ? "remount: error " : "remount: / is read-write ");
		outi(r < 0 ? -r : r);
		out("\n");
	} else if (seq(line, "mkdir")) {
		long r = sys3(SYS_mkdir, arg ? arg : "", 0755, 0);
		out(r < 0 ? "mkdir: error " : "mkdir: ok ");
		outi(r < 0 ? -r : r);
		out("\n");
	} else if (seq(line, "sleep")) {
		cmd_sleep(arg);
	} else if (seq(line, "help")) {
		help();
	} else if (seq(line, "exit")) {
		out("fire_sh: exit\n");
		sys3(SYS_exit, arg ? (long)atou(arg) : 0, 0, 0);
	} else {
		out("fire_sh: unknown command: ");
		out(line);
		out("\n");
	}
}

/* One string, not an array of pointers: there is no dyld to apply pointer fixups in __DATA. */
static const char script[] =
    "help\n"
    "echo DarwinFire shell: PID 1 on open-source XNU, Redmi 12 fire / MT6768\n"
    "uname -a\n"
    "hw\n"
    "pid\n"
    "uptime\n"
    "ls /\n"
    "ls /etc\n"
    "cat /etc/motd\n"
    "remount\n"
    "write /tmp/hello.txt written on HFS+ by PID 1\n"
    "mkdir /tmp/newdir\n"
    "ls /tmp\n"
    "cat /tmp/hello.txt\n"
    "thread\n"
    "ls /bin\n"
    "run /bin/hello one two\n"
    "run /bin/ltest\n"
    "run /bin/dash /etc/test.sh\n"
    "sleep 2\n"
    "uptime\n"
    "echo script done\n";

void cstart(void);
/* LC_UNIXTHREAD enters with SP at argc, only 8-byte aligned; AArch64 faults on a misaligned SP. */
__asm__(".globl _start\n"
    "_start:\n"
    "\tmov x9, sp\n"
    "\tand sp, x9, #~15\n"
    "\tb _cstart\n");

void
cstart(void)
{
	con = sys3(SYS_open, "/dev/console", O_RDWR | O_NOCTTY, 0);
	/* stdin/stdout/stderr for the programs started by `run` (dup2 = syscall 90) */
	for (long fd = 0; fd < 3; fd++) {
		if (fd != con) {
			sys3(90, con, fd, 0);
		}
	}
	out("\n**********************************************\n");
	out("*  DarwinFire: hello from userland (PID 1)   *\n");
	out("*  fire_sh v2 on open-source XNU, MT6768     *\n");
	out("**********************************************\n\n");

	char line[256];
	for (const char *p = script; *p;) {
		size_t n = 0;
		while (p[n] && p[n] != '\n' && n < sizeof(line) - 1) {
			n++;
		}
		memcpy(line, p, n);
		line[n] = 0;
		p += n + (p[n] == '\n');
		out("fire# ");
		out(line);
		out("\n");
		run(line);
	}

	for (;;) {
		out("fire# ");
		flush();
		long n = sys3(SYS_read, con, line, sizeof(line) - 1);
		if (n <= 0) {
			out("\nfire_sh: console read returned ");
			outi(n);
			out(", exiting with 0x42\n");
			sys3(SYS_exit, 0x42, 0, 0);
		}
		line[n] = 0;
		for (long k = 0; k < n; k++) {
			if (line[k] == '\n' || line[k] == '\r') {
				line[k] = 0;
			}
		}
		run(line);
	}
}

/*
 * firelibc implementation (see firelibc.h). Built with -ffreestanding; no pointers stored in
 * initialized data, so static binaries need no fixups (there is no dyld).
 */
#include "firelibc.h"
#include <stdarg.h>

int errno;
char **environ;

/* ---- crt0: LC_UNIXTHREAD enters with sp -> argc, argv..., 0, envp..., 0, apple..., 0 ----
 * sp is only 8-byte aligned there; AArch64 faults on a misaligned sp, so align before any C. */
int main(int argc, char **argv, char **envp);
static void ctype_init(void);
void __fire_start(long *sp) __attribute__((noreturn));
__asm__(".globl _start\n"
    "_start:\n"
    "\tmov x0, sp\n"
    "\tand sp, x0, #~15\n"
    "\tb ___fire_start\n");
void
__fire_start(long *sp)
{
	int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;
	environ = envp;
	ctype_init();
	exit(main(argc, argv, envp));
}

/* ---- syscalls: svc #0x80, number in x16, carry set => x0 is errno ---- */
static long
sc(long n, long a, long b, long c, long d, long e, long f, long *x1out)
{
	register long x16 __asm__("x16") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	unsigned long nzcv;
	__asm__ volatile ("svc #0x80\n\tmrs %[v], nzcv"
	    : "+r"(x0), "+r"(x1), [v] "=r"(nzcv)
	    : "r"(x16), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
	    : "memory", "cc");
	if (x1out) {
		*x1out = x1;
	}
	if (nzcv & (1ul << 29)) {
		errno = (int)x0;
		return -1;
	}
	return x0;
}
#define S0(n)             sc((n), 0, 0, 0, 0, 0, 0, 0)
#define S1(n, a)          sc((n), (long)(a), 0, 0, 0, 0, 0, 0)
#define S2(n, a, b)       sc((n), (long)(a), (long)(b), 0, 0, 0, 0, 0)
#define S3(n, a, b, c)    sc((n), (long)(a), (long)(b), (long)(c), 0, 0, 0, 0)
#define S4(n, a, b, c, d) sc((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0, 0)

ssize_t read(int fd, void *buf, size_t n) { return S3(3, fd, buf, n); }
ssize_t write(int fd, const void *buf, size_t n) { return S3(4, fd, buf, n); }
int
open(const char *path, int flags, ...)
{
	va_list ap;
	va_start(ap, flags);
	int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
	va_end(ap);
	return (int)S3(5, path, flags, mode);
}
int close(int fd) { return (int)S1(6, fd); }
void
_exit(int status)
{
	S1(1, status);
	for (;;) {
	}
}
void exit(int status) { _exit(status); }
pid_t getpid(void) { return (pid_t)S0(20); }
pid_t getppid(void) { return (pid_t)S0(39); }
/* Darwin fork: x1 == 1 in the child, 0 in the parent (x0 = child pid there). */
pid_t
fork(void)
{
	long x1 = 0;
	long r = sc(2, 0, 0, 0, 0, 0, 0, &x1);
	if (r < 0) {
		return -1;
	}
	return x1 ? 0 : (pid_t)r;
}
int execve(const char *path, char *const argv[], char *const envp[]) { return (int)S3(59, path, argv, envp); }
pid_t waitpid(pid_t pid, int *status, int options) { return (pid_t)S4(7, pid, status, options, 0); }
int dup2(int from, int to) { return (int)S2(90, from, to); }
int chdir(const char *path) { return (int)S1(12, path); }
int mkdir(const char *path, mode_t mode) { return (int)S2(136, path, mode); }
int unlink(const char *path) { return (int)S1(10, path); }
off_t lseek(int fd, off_t off, int whence) { return S3(199, fd, off, whence); }

/* ---- string (also what -O2 may call implicitly) ---- */
size_t
strlen(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}
int
strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}
int
strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b || !*a) {
			return (unsigned char)*a - (unsigned char)*b;
		}
	}
	return 0;
}
char *
strcpy(char *d, const char *s)
{
	char *r = d;
	while ((*d++ = *s++)) {
	}
	return r;
}
char *
strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c) {
			return (char *)s;
		}
		if (!*s) {
			return NULL;
		}
	}
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
void *
memmove(void *d, const void *s, size_t n)
{
	unsigned char *p = d;
	const unsigned char *q = s;
	if (p < q) {
		while (n--) {
			*p++ = *q++;
		}
	} else {
		while (n--) {
			p[n] = q[n];
		}
	}
	return d;
}
void *
memset(void *d, int c, size_t n)
{
	unsigned char *p = d;
	while (n--) {
		*p++ = (unsigned char)c;
	}
	return d;
}
/* clang emits bzero calls for zero-initialization on Darwin targets */
void
bzero(void *d, size_t n)
{
	memset(d, 0, n);
}
int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++) {
		if (*p != *q) {
			return *p - *q;
		}
	}
	return 0;
}
/* switch, not a table: a table of string pointers would need fixups */
const char *
strerror(int e)
{
	switch (e) {
	case 1: return "Operation not permitted";
	case 2: return "No such file or directory";
	case 9: return "Bad file descriptor";
	case 12: return "Cannot allocate memory";
	case 13: return "Permission denied";
	case 14: return "Bad address";
	case 17: return "File exists";
	case 20: return "Not a directory";
	case 21: return "Is a directory";
	case 22: return "Invalid argument";
	case 30: return "Read-only file system";
	default: return "Unknown error";
	}
}
char *
getenv(const char *name)
{
	size_t n = strlen(name);
	for (char **e = environ; e && *e; e++) {
		if (!strncmp(*e, name, n) && (*e)[n] == '=') {
			return *e + n + 1;
		}
	}
	return NULL;
}

/* ---- memory: every block has a 16-byte header holding its usable size ---- */
struct blk {
	size_t size;
	struct blk *next; /* valid only while on the free list */
};
static struct blk *freelist;
static char *arena, *arena_end;
#define ARENA (1ul << 20)
static void *
mmap_anon(size_t n)
{
	long r = sc(197, 0, (long)n, 3 /* RW */, 0x1002 /* ANON|PRIVATE */, -1, 0, 0);
	return r == -1 ? NULL : (void *)r;
}
void *
malloc(size_t n)
{
	n = (n + 15) & ~15ul;
	for (struct blk **pp = &freelist; *pp; pp = &(*pp)->next) {
		if ((*pp)->size >= n) {
			struct blk *b = *pp;
			*pp = b->next;
			return (char *)b + 16;
		}
	}
	if (n + 16 > (size_t)(arena_end - arena)) {
		size_t want = n + 16 > ARENA ? (n + 16 + 0x3fff) & ~0x3ffful : ARENA;
		char *m = mmap_anon(want);
		if (!m) {
			errno = 12;
			return NULL;
		}
		arena = m; /* rest of the old arena is abandoned (shortcut) */
		arena_end = m + want;
	}
	struct blk *b = (struct blk *)arena;
	arena += n + 16;
	b->size = n;
	return (char *)b + 16;
}
void
free(void *p)
{
	if (p) {
		struct blk *b = (struct blk *)((char *)p - 16);
		b->next = freelist;
		freelist = b;
	}
}
void *
calloc(size_t a, size_t b)
{
	void *p = malloc(a * b);
	if (p) {
		memset(p, 0, a * b);
	}
	return p;
}
void *
realloc(void *p, size_t n)
{
	if (!p) {
		return malloc(n);
	}
	size_t old = ((struct blk *)((char *)p - 16))->size;
	if (n <= old) {
		return p;
	}
	void *q = malloc(n);
	if (q) {
		memcpy(q, p, old);
		free(p);
	}
	return q;
}
char *
strdup(const char *s)
{
	char *d = malloc(strlen(s) + 1);
	return d ? strcpy(d, s) : NULL;
}
long
strtol(const char *s, char **end, int base)
{
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	int neg = *s == '-';
	if (*s == '-' || *s == '+') {
		s++;
	}
	if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = *s == '0' ? 8 : 10;
	}
	long v = 0;
	for (;; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'z' ? *s - 'a' + 10 :
		    *s >= 'A' && *s <= 'Z' ? *s - 'A' + 10 : 99;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	if (end) {
		*end = (char *)s;
	}
	return neg ? -v : v;
}
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }

/* ---- files: the 64-bit-inode syscalls; struct stat above is their layout ---- */
int stat(const char *path, struct stat *st) { return (int)S2(338, path, st); }
int fstat(int fd, struct stat *st) { return (int)S2(339, fd, st); }
int lstat(const char *path, struct stat *st) { return (int)S2(340, path, st); }

struct DIR {
	int fd, pos, len;
	long base;
	char buf[4096];
};
DIR *
opendir(const char *path)
{
	int fd = open(path, O_RDONLY | 0x100000 /* O_DIRECTORY */);
	if (fd < 0) {
		return NULL;
	}
	DIR *d = calloc(1, sizeof(*d));
	if (!d) {
		close(fd);
		return NULL;
	}
	d->fd = fd;
	return d;
}
struct dirent *
readdir(DIR *d)
{
	if (d->pos >= d->len) {
		long n = S4(344 /* getdirentries64 */, d->fd, d->buf, sizeof(d->buf), &d->base);
		if (n <= 0) {
			return NULL;
		}
		d->len = (int)n;
		d->pos = 0;
	}
	struct dirent *e = (struct dirent *)(d->buf + d->pos);
	d->pos += e->d_reclen;
	return e;
}
int
closedir(DIR *d)
{
	int r = close(d->fd);
	free(d);
	return r;
}

/* ---- setjmp/longjmp ---- */
__asm__(".globl _setjmp\n_setjmp:\n"
    "\tstp x19, x20, [x0, #0]\n\tstp x21, x22, [x0, #16]\n\tstp x23, x24, [x0, #32]\n"
    "\tstp x25, x26, [x0, #48]\n\tstp x27, x28, [x0, #64]\n\tstp x29, x30, [x0, #80]\n"
    "\tmov x2, sp\n\tstr x2, [x0, #96]\n"
    "\tstp d8, d9, [x0, #104]\n\tstp d10, d11, [x0, #120]\n\tstp d12, d13, [x0, #136]\n\tstp d14, d15, [x0, #152]\n"
    "\tmov x0, #0\n\tret\n"
    ".globl _longjmp\n_longjmp:\n"
    "\tldp x19, x20, [x0, #0]\n\tldp x21, x22, [x0, #16]\n\tldp x23, x24, [x0, #32]\n"
    "\tldp x25, x26, [x0, #48]\n\tldp x27, x28, [x0, #64]\n\tldp x29, x30, [x0, #80]\n"
    "\tldr x2, [x0, #96]\n\tmov sp, x2\n"
    "\tldp d8, d9, [x0, #104]\n\tldp d10, d11, [x0, #120]\n\tldp d12, d13, [x0, #136]\n\tldp d14, d15, [x0, #152]\n"
    "\tcmp w1, #0\n\tcsinc w0, w1, wzr, ne\n\tret\n");

/* ---- signals: the kernel enters our trampoline, which runs the handler and calls sigreturn
 * with the token the kernel passed in x5 ---- */
static void
sigtramp(void (*h)(int), int style, int sig, void *info, void *uctx, long token)
{
	(void)info;
	h(sig);
	sc(184 /* sigreturn */, (long)uctx, style, token, 0, 0, 0, 0);
	for (;;) {
	}
}
void (*signal(int sig, void (*h)(int)))(int)
{
	struct {
		void *handler, *tramp;
		unsigned mask;
		int flags;
	} n = { (void *)h, (void *)sigtramp, 0, 0x2 /* SA_RESTART */ };
	struct {
		void *handler;
		unsigned mask;
		int flags;
	} o;
	if (S3(46 /* sigaction */, sig, &n, &o) < 0) {
		return (void (*)(int)) - 1;
	}
	return (void (*)(int))o.handler;
}
int kill(pid_t pid, int sig) { return (int)S3(37, pid, sig, 1 /* posix */); }

/* ---- formatted output (buffered per call, so lines are not split into 1-byte writes) ---- */
/* Sink is either an fd (buffered) or a caller buffer of cap bytes (snprintf semantics). */
struct obuf {
	int fd;
	int len;
	int total;
	char *dst;
	size_t cap;
	char b[512];
};
static void
ob_flush(struct obuf *o)
{
	if (o->len && !o->dst) {
		write(o->fd, o->b, (size_t)o->len);
	}
	o->len = 0;
}
static void
ob_put(struct obuf *o, char c)
{
	if (o->dst) {
		if ((size_t)o->total + 1 < o->cap) {
			o->dst[o->total] = c;
		}
		o->total++;
		return;
	}
	if (o->len == (int)sizeof(o->b)) {
		ob_flush(o);
	}
	o->b[o->len++] = c;
	o->total++;
}
static void
ob_field(struct obuf *o, const char *s, int n, int width, int left, char pad)
{
	for (; !left && width > n; width--) {
		ob_put(o, pad);
	}
	for (int i = 0; i < n; i++) {
		ob_put(o, s[i]);
	}
	for (; left && width > n; width--) {
		ob_put(o, ' ');
	}
}
static int
vfmt(struct obuf *o, const char *f, va_list ap)
{
	for (; *f; f++) {
		if (*f != '%') {
			ob_put(o, *f);
			continue;
		}
		int left = 0, zero = 0, width = 0, prec = -1, lng = 0, plus = 0;
		for (f++;; f++) {
			if (*f == '-') {
				left = 1;
			} else if (*f == '0') {
				zero = 1;
			} else if (*f == '+') {
				plus = 1;
			} else if (*f != ' ' && *f != '#') {
				break;
			}
		}
		if (*f == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			f++;
		}
		while (*f >= '0' && *f <= '9') {
			width = width * 10 + *f++ - '0';
		}
		if (*f == '.') {
			prec = 0;
			if (*++f == '*') {
				prec = va_arg(ap, int);
				f++;
			}
			while (*f >= '0' && *f <= '9') {
				prec = prec * 10 + *f++ - '0';
			}
		}
		for (; *f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 'q' || *f == 't'; f++) {
			lng |= *f != 'h';
		}
		char t[24];
		int i = sizeof(t);
		switch (*f) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			s = s ? s : "(null)";
			int n = 0;
			while (s[n] && (prec < 0 || n < prec)) {
				n++;
			}
			ob_field(o, s, n, width, left, ' ');
			break;
		}
		case 'c':
			t[0] = (char)va_arg(ap, int);
			ob_field(o, t, 1, width, left, ' ');
			break;
		case 'd':
		case 'i':
		case 'u':
		case 'x':
		case 'X':
		case 'o':
		case 'p': {
			unsigned long v;
			int neg = 0;
			unsigned base = *f == 'o' ? 8 : (*f == 'x' || *f == 'X' || *f == 'p') ? 16 : 10;
			if (*f == 'p') {
				v = (unsigned long)va_arg(ap, void *);
			} else if (*f == 'd' || *f == 'i') {
				long s = lng ? va_arg(ap, long) : va_arg(ap, int);
				neg = s < 0;
				v = neg ? -(unsigned long)s : (unsigned long)s;
			} else {
				v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
			}
			const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			do {
				t[--i] = dig[v % base];
				v /= base;
			} while (v);
			while (prec > 0 && (int)sizeof(t) - i < prec && i > 2) {
				t[--i] = '0';
			}
			if (*f == 'p') {
				t[--i] = 'x';
				t[--i] = '0';
			}
			if (neg || plus) {
				if (zero && !left) {
					ob_put(o, neg ? '-' : '+');
					width--;
				} else {
					t[--i] = neg ? '-' : '+';
				}
			}
			ob_field(o, t + i, (int)sizeof(t) - i, width, left, zero && !left ? '0' : ' ');
			break;
		}
		case '%':
			ob_put(o, '%');
			break;
		default:
			ob_put(o, '%');
			ob_put(o, *f);
		}
	}
	ob_flush(o);
	if (o->dst && o->cap) {
		o->dst[(size_t)o->total < o->cap ? (size_t)o->total : o->cap - 1] = 0;
	}
	return o->total;
}
static int
vfd(int fd, const char *f, va_list ap)
{
	struct obuf o;
	o.fd = fd;
	o.len = o.total = 0;
	o.dst = NULL;
	o.cap = 0;
	return vfmt(&o, f, ap);
}
int
vsnprintf(char *buf, size_t cap, const char *f, va_list ap)
{
	struct obuf o;
	o.fd = -1;
	o.len = o.total = 0;
	o.dst = buf;
	o.cap = cap;
	return vfmt(&o, f, ap);
}
int
snprintf(char *buf, size_t cap, const char *f, ...)
{
	va_list ap;
	va_start(ap, f);
	int r = vsnprintf(buf, cap, f, ap);
	va_end(ap);
	return r;
}

/* ---- the rest of what dash needs ---- */
int *__error(void) { return &errno; }
/* stack probe: pages are mapped on demand by the kernel anyway, nothing to touch */
__asm__(".globl ___chkstk_darwin\n___chkstk_darwin:\n\tret\n");

/* ctype: the SDK macros read _DefaultRuneLocale.__runetype[c] (ASCII only here) */
struct {
	char magic[8], enc[32];
	void *sget, *sput;
	int invalid;
	unsigned runetype[256];
	int maplower[256], mapupper[256];
	char rest[256];
} _DefaultRuneLocale;
static void
ctype_init(void)
{
	for (int c = 0; c < 256; c++) {
		unsigned t = 0;
		int up = c >= 'A' && c <= 'Z', lo = c >= 'a' && c <= 'z', dg = c >= '0' && c <= '9';
		if (up) {
			t |= 0x100 | 0x8000; /* A U */
		}
		if (lo) {
			t |= 0x100 | 0x1000; /* A L */
		}
		if (dg) {
			t |= 0x400 | (unsigned)(c - '0'); /* D + digit value */
		}
		if (dg || ((c | 32) >= 'a' && (c | 32) <= 'f')) {
			t |= 0x10000; /* X */
		}
		if (c < 32 || c == 127) {
			t |= 0x200; /* C */
		}
		if (c == ' ' || (c >= 9 && c <= 13)) {
			t |= 0x4000; /* S */
		}
		if (c == ' ' || c == '\t') {
			t |= 0x20000; /* B */
		}
		if (c >= 32 && c < 127) {
			t |= 0x40000; /* R */
			if (c != ' ') {
				t |= 0x800; /* G */
				if (!up && !lo && !dg) {
					t |= 0x2000; /* P */
				}
			}
		}
		_DefaultRuneLocale.runetype[c] = t;
		_DefaultRuneLocale.maplower[c] = up ? c + 32 : c;
		_DefaultRuneLocale.mapupper[c] = lo ? c - 32 : c;
	}
}
unsigned long __maskrune(int c, unsigned long f) { return (c >= 0 && c < 256) ? _DefaultRuneLocale.runetype[c] & f : 0; }
int __toupper(int c) { return (c >= 0 && c < 256) ? _DefaultRuneLocale.mapupper[c] : c; }
int __tolower(int c) { return (c >= 0 && c < 256) ? _DefaultRuneLocale.maplower[c] : c; }

void
abort(void)
{
	kill(getpid(), 6);
	_exit(134);
}
int raise(int sig) { return kill(getpid(), sig); }
int killpg(pid_t pg, int sig) { return kill(-pg, sig); }
unsigned getuid(void) { return (unsigned)S0(24); }
unsigned geteuid(void) { return (unsigned)S0(25); }
unsigned getgid(void) { return (unsigned)S0(47); }
unsigned getegid(void) { return (unsigned)S0(43); }
pid_t getpgrp(void) { return (pid_t)S0(81); }
int setpgid(pid_t p, pid_t g) { return (int)S2(82, p, g); }
mode_t umask(mode_t m) { return (mode_t)S1(60, m); }
int getrlimit(int r, void *l) { return (int)S2(194, r, l); }
int setrlimit(int r, const void *l) { return (int)S2(195, r, l); }
int
fcntl(int fd, int cmd, ...)
{
	va_list ap;
	va_start(ap, cmd);
	long a = va_arg(ap, long);
	va_end(ap);
	return (int)S3(92, fd, cmd, a);
}
int ioctl(int fd, unsigned long req, void *a) { return (int)S3(54, fd, req, a); }
int
isatty(int fd)
{
	char t[72]; /* struct termios */
	return ioctl(fd, 0x40487413 /* TIOCGETA */, t) == 0;
}
pid_t
tcgetpgrp(int fd)
{
	int g;
	return ioctl(fd, 0x40047477 /* TIOCGPGRP */, &g) < 0 ? -1 : g;
}
int tcsetpgrp(int fd, pid_t g) { return ioctl(fd, 0x80047476 /* TIOCSPGRP */, &g); }
/* pipe(2) returns the read end in x0 and the write end in x1 */
int
pipe(int fd[2])
{
	long x1 = 0;
	long r = sc(42, 0, 0, 0, 0, 0, 0, &x1);
	if (r < 0) {
		return -1;
	}
	fd[0] = (int)r;
	fd[1] = (int)x1;
	return 0;
}
pid_t vfork(void) { return fork(); } /* shortcut: real fork, costs a copy; fine for a shell this size */
pid_t wait3(int *st, int opt, void *ru) { return (pid_t)S4(7, -1, st, opt, ru); }
/* no getcwd syscall on Darwin: ask the kernel for the path of "." */
char *
getcwd(char *buf, size_t n)
{
	char p[1024];
	int fd = open(".", O_RDONLY);
	if (fd < 0) {
		return NULL;
	}
	int r = fcntl(fd, 50 /* F_GETPATH */, p);
	close(fd);
	if (r < 0) {
		return NULL;
	}
	if (strlen(p) + 1 > n) {
		errno = 34;
		return NULL;
	}
	return strcpy(buf, p);
}
void *getpwnam(const char *n) { (void)n; return NULL; } /* no user database yet */

typedef unsigned sigset_t;
struct sigaction {
	void (*handler)(int);
	sigset_t mask;
	int flags;
};
int
sigaction(int sig, const struct sigaction *a, struct sigaction *old)
{
	struct {
		void *handler, *tramp;
		sigset_t mask;
		int flags;
	} k;
	if (a) {
		k.handler = (void *)a->handler;
		k.tramp = (void *)sigtramp;
		k.mask = a->mask;
		k.flags = a->flags;
	}
	return (int)S3(46, sig, a ? &k : NULL, old);
}
int sigprocmask(int how, const sigset_t *s, sigset_t *o) { return (int)S3(48, how, s, o); }
int
sigsetmask(int m)
{
	sigset_t s = (sigset_t)m, o = 0;
	sigprocmask(3 /* SIG_SETMASK */, &s, &o);
	return (int)o;
}
int sigsuspend(const sigset_t *s) { return (int)S1(111, *s); }
const char *
strsignal(int s)
{
	switch (s) {
	case 1: return "Hangup";
	case 2: return "Interrupt";
	case 3: return "Quit";
	case 6: return "Abort trap";
	case 9: return "Killed";
	case 10: return "Bus error";
	case 11: return "Segmentation fault";
	case 13: return "Broken pipe";
	case 15: return "Terminated";
	default: return "Signal";
	}
}

void *
memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;
	for (; n; n--, p++) {
		if (*p == (unsigned char)c) {
			return (void *)p;
		}
	}
	return NULL;
}
void
memset_pattern16(void *d, const void *pat, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		((unsigned char *)d)[i] = ((const unsigned char *)pat)[i & 15];
	}
}
char *
stpcpy(char *d, const char *s)
{
	while ((*d = *s++)) {
		d++;
	}
	return d;
}
char *
stpncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++) {
		d[i] = s[i];
	}
	char *r = d + i;
	for (; i < n; i++) {
		d[i] = 0;
	}
	return r;
}
int
strcasecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = __tolower((unsigned char)*a), y = __tolower((unsigned char)*b);
		if (x != y || !x) {
			return x - y;
		}
	}
}
size_t
strspn(const char *s, const char *set)
{
	size_t n = 0;
	while (s[n] && strchr(set, s[n])) {
		n++;
	}
	return n;
}
size_t
strcspn(const char *s, const char *set)
{
	size_t n = 0;
	while (s[n] && !strchr(set, s[n])) {
		n++;
	}
	return n;
}
char *
strpbrk(const char *s, const char *set)
{
	s += strcspn(s, set);
	return *s ? (char *)s : NULL;
}
char *
strstr(const char *h, const char *n)
{
	size_t l = strlen(n);
	for (; *h; h++) {
		if (!strncmp(h, n, l)) {
			return (char *)h;
		}
	}
	return l ? NULL : (char *)h;
}
char *
strtok(char *s, const char *sep)
{
	static char *last;
	if (!s) {
		s = last;
	}
	if (!s) {
		return NULL;
	}
	s += strspn(s, sep);
	if (!*s) {
		return last = NULL;
	}
	char *e = s + strcspn(s, sep);
	last = *e ? e + 1 : NULL;
	*e = 0;
	return s;
}
long strtoimax(const char *s, char **e, int b) { return strtol(s, e, b); }
unsigned long strtoumax(const char *s, char **e, int b) { return (unsigned long)strtol(s, e, b); }
double
strtod(const char *s, char **end)
{
	const char *p = s;
	while (*p == ' ' || *p == '\t') {
		p++;
	}
	int neg = *p == '-';
	p += *p == '-' || *p == '+';
	double v = 0, scale = 1;
	const char *d0 = p;
	for (; *p >= '0' && *p <= '9'; p++) {
		v = v * 10 + (*p - '0');
	}
	if (*p == '.') {
		for (p++; *p >= '0' && *p <= '9'; p++) {
			v = v * 10 + (*p - '0');
			scale *= 10;
		}
	}
	if (p == d0) {
		p = s; /* no digits: nothing converted */
	} else if (*p == 'e' || *p == 'E') {
		char *q;
		long x = strtol(p + 1, &q, 10);
		if (q != p + 1) {
			p = q;
			for (; x > 0; x--) {
				v *= 10;
			}
			for (; x < 0; x++) {
				scale *= 10;
			}
		}
	}
	if (end) {
		*end = (char *)p;
	}
	v /= scale;
	return neg ? -v : v;
}
int faccessat(int fd, const char *path, int mode, int flag) { return (int)S4(466, fd, path, mode, flag); }
long sysconf(int name) { return name == 3 /* _SC_CLK_TCK */ ? 100 : -1; }
/* no times syscall on Darwin: user/sys time from getrusage, in 1/100 s */
unsigned long
times(unsigned long t[4])
{
	long ru[18], rc[18]; /* struct rusage: utime, stime timevals first */
	S2(117, 0, ru);
	S2(117, -1, rc);
	t[0] = (unsigned long)(ru[0] * 100 + (int)ru[1] / 10000);
	t[1] = (unsigned long)(ru[2] * 100 + (int)ru[3] / 10000);
	t[2] = (unsigned long)(rc[0] * 100 + (int)rc[1] / 10000);
	t[3] = (unsigned long)(rc[2] * 100 + (int)rc[3] / 10000);
	return 0;
}

void *
bsearch(const void *key, const void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		const char *p = (const char *)base + mid * sz;
		int c = cmp(key, p);
		if (!c) {
			return (void *)p;
		}
		if (c < 0) {
			hi = mid;
		} else {
			lo = mid + 1;
		}
	}
	return NULL;
}
/* shortcut: insertion sort, O(n^2); dash only sorts short lists (e.g. `set` output) */
void
qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
	char *b = base, tmp[256];
	if (sz > sizeof(tmp)) {
		return;
	}
	for (size_t i = 1; i < n; i++) {
		memcpy(tmp, b + i * sz, sz);
		size_t j = i;
		for (; j && cmp(b + (j - 1) * sz, tmp) > 0; j--) {
			memcpy(b + j * sz, b + (j - 1) * sz, sz);
		}
		memcpy(b + j * sz, tmp, sz);
	}
}

/* fnmatch: * ? [set] [!set] and backslash escapes; FNM_NOESCAPE=1, FNM_PERIOD=4 */
int
fnmatch(const char *p, const char *s, int flags)
{
	const char *s0 = s;
	for (;; p++, s++) {
		switch (*p) {
		case 0:
			return *s ? 1 : 0;
		case '?':
			if (!*s || ((flags & 4) && *s == '.' && s == s0)) {
				return 1;
			}
			break;
		case '*':
			if ((flags & 4) && *s == '.' && s == s0) {
				return 1;
			}
			while (*p == '*') {
				p++;
			}
			for (;; s++) {
				if (!fnmatch(p, s, flags & ~4)) {
					return 0;
				}
				if (!*s) {
					return 1;
				}
			}
		case '[': {
			if (!*s) {
				return 1;
			}
			const char *q = p + 1;
			int neg = *q == '!' || *q == '^', ok = 0;
			q += neg;
			do {
				char a = *q, z = a;
				if (!a) {
					return 1;
				}
				if (q[1] == '-' && q[2] && q[2] != ']') {
					z = q[2];
					q += 2;
				}
				ok |= *s >= a && *s <= z;
			} while (*++q != ']');
			if (ok == neg) {
				return 1;
			}
			p = q;
			break;
		}
		case '\\':
			if (!(flags & 1) && p[1]) {
				p++;
			}
			/* fallthrough */
		default:
			if (*p != *s) {
				return 1;
			}
		}
	}
}
int
printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfd(1, fmt, ap);
	va_end(ap);
	return r;
}
int
dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfd(fd, fmt, ap);
	va_end(ap);
	return r;
}
int
puts(const char *s)
{
	return printf("%s\n", s);
}

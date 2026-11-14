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

/* ---- formatted output (buffered per call, so lines are not split into 1-byte writes) ---- */
struct obuf {
	int fd;
	int len;
	int total;
	char b[512];
};
static void
ob_flush(struct obuf *o)
{
	if (o->len) {
		write(o->fd, o->b, (size_t)o->len);
		o->len = 0;
	}
}
static void
ob_put(struct obuf *o, char c)
{
	if (o->len == (int)sizeof(o->b)) {
		ob_flush(o);
	}
	o->b[o->len++] = c;
	o->total++;
}
static void
ob_num(struct obuf *o, unsigned long v, unsigned base, int neg)
{
	char t[24];
	int i = 0;
	do {
		t[i++] = "0123456789abcdef"[v % base];
		v /= base;
	} while (v);
	if (neg) {
		ob_put(o, '-');
	}
	while (i) {
		ob_put(o, t[--i]);
	}
}
static int
vfmt(int fd, const char *f, va_list ap)
{
	struct obuf o = { fd, 0, 0, { 0 } };
	for (; *f; f++) {
		if (*f != '%') {
			ob_put(&o, *f);
			continue;
		}
		int lng = 0;
		f++;
		while (*f == 'l') {
			lng = 1;
			f++;
		}
		switch (*f) {
		case 's': {
			const char *s = va_arg(ap, const char *);
			for (s = s ? s : "(null)"; *s; s++) {
				ob_put(&o, *s);
			}
			break;
		}
		case 'c':
			ob_put(&o, (char)va_arg(ap, int));
			break;
		case 'd': {
			long v = lng ? va_arg(ap, long) : va_arg(ap, int);
			ob_num(&o, (unsigned long)(v < 0 ? -v : v), 10, v < 0);
			break;
		}
		case 'u':
			ob_num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0);
			break;
		case 'x':
			ob_num(&o, lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0);
			break;
		case 'p':
			ob_put(&o, '0');
			ob_put(&o, 'x');
			ob_num(&o, (unsigned long)va_arg(ap, void *), 16, 0);
			break;
		case '%':
			ob_put(&o, '%');
			break;
		default:
			ob_put(&o, '%');
			ob_put(&o, *f);
		}
	}
	ob_flush(&o);
	return o.total;
}
int
printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfmt(1, fmt, ap);
	va_end(ap);
	return r;
}
int
dprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = vfmt(fd, fmt, ap);
	va_end(ap);
	return r;
}
int
puts(const char *s)
{
	return printf("%s\n", s);
}

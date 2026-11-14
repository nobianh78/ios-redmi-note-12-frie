/*
 * firelibc: a minimal static C library for userland on the fire XNU port (open-source XNU, no
 * Apple libSystem, no dyld). Raw Darwin syscalls; enough for small tools and, later, a real shell.
 * shortcut: grows only as programs need it; not a full POSIX libc.
 */
#ifndef FIRELIBC_H
#define FIRELIBC_H

typedef unsigned long size_t;
typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
#define NULL ((void *)0)

extern int errno;
extern char **environ;

/* syscalls (return -1 and set errno on failure) */
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int open(const char *path, int flags, ...);
int close(int fd);
void _exit(int status) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));
pid_t getpid(void);
pid_t getppid(void);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
int dup2(int from, int to);
int chdir(const char *path);
int mkdir(const char *path, mode_t mode);
int unlink(const char *path);
off_t lseek(int fd, off_t off, int whence);

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0200
#define O_TRUNC  0x0400
#define O_NOCTTY 0x20000

#define WIFEXITED(s)   (((s) & 0177) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)

/* string */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
const char *strerror(int e);

/* output: printf-style to stdout/stderr (%s %d %u %x %p %c %%, %l modifier) */
int printf(const char *fmt, ...);
int dprintf(int fd, const char *fmt, ...);
int puts(const char *s);
int snprintf(char *buf, size_t cap, const char *fmt, ...);
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *));
char *getenv(const char *name);

/* memory (mmap-backed; shortcut: first-fit free list, no coalescing; fine for small tools) */
void *malloc(size_t n);
void free(void *p);
void *calloc(size_t a, size_t b);
void *realloc(void *p, size_t n);
char *strdup(const char *s);
long strtol(const char *s, char **end, int base);
int atoi(const char *s);

/* files */
struct stat {
	int st_dev;
	unsigned short st_mode, st_nlink;
	unsigned long st_ino;
	unsigned st_uid, st_gid;
	int st_rdev;
	long st_atime, st_atimensec, st_mtime, st_mtimensec, st_ctime, st_ctimensec, st_btime, st_btimensec;
	long st_size, st_blocks;
	int st_blksize;
	unsigned st_flags, st_gen;
	int st_lspare;
	long st_qspare[2];
};
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);

struct dirent {
	unsigned long d_ino, d_seekoff;
	unsigned short d_reclen, d_namlen;
	unsigned char d_type;
	char d_name[1024];
};
typedef struct DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);

/* non-local jumps: x19-x30, sp, d8-d15 */
typedef long jmp_buf[22];
int setjmp(jmp_buf b) __attribute__((returns_twice));
void longjmp(jmp_buf b, int v) __attribute__((noreturn));

/* signals */
#define SIGINT  2
#define SIGKILL 9
#define SIGTERM 15
#define SIGCHLD 20
#define SIGUSR1 30
#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
void (*signal(int sig, void (*h)(int)))(int);
int kill(pid_t pid, int sig);

#endif

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
char *getenv(const char *name);

#endif

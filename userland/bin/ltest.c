/* /bin/ltest: self-check for firelibc milestone B (malloc, dirs, stat, setjmp, signals). */
#include "../libc/firelibc.h"

static int fails;
static volatile int got;
static void on_usr1(int s) { got = s; }
static void
check(const char *what, int ok)
{
	printf("  %s: %s\n", what, ok ? "ok" : "FAIL");
	fails += !ok;
}

int
main(void)
{
	char *a = malloc(100), *b = malloc(5000);
	strcpy(a, "abc");
	a = realloc(a, 300000); /* forces a second arena */
	check("malloc/realloc", a && b && !strcmp(a, "abc"));
	free(b);
	check("free+reuse", malloc(4000) == b);
	check("strtol", strtol("-0x1f", NULL, 0) == -31 && atoi("42") == 42);

	struct stat st;
	check("stat /etc/motd", stat("/etc/motd", &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0);
	check("stat /bin", stat("/bin", &st) == 0 && S_ISDIR(st.st_mode));

	DIR *d = opendir("/");
	int n = 0, seen_etc = 0;
	for (struct dirent *e; d && (e = readdir(d));) {
		n++;
		seen_etc |= !strcmp(e->d_name, "etc");
	}
	check("readdir /", d && n > 3 && seen_etc);
	if (d) {
		closedir(d);
	}

	jmp_buf j;
	volatile int k = 0;
	if (setjmp(j) == 0) {
		k = 1;
		longjmp(j, 5);
	}
	check("setjmp/longjmp", k == 1);

	signal(SIGUSR1, on_usr1);
	kill(getpid(), SIGUSR1);
	check("signal", got == SIGUSR1);

	printf("ltest: %d failed\n", fails);
	return fails;
}

/* /bin/hello: first program started by fire_sh through fork + execve, linked with firelibc. */
#include "../libc/firelibc.h"

int
main(int argc, char **argv, char **envp)
{
	(void)envp;
	printf("hello from /bin/hello (pid %d, parent %d), argc=%d\n", getpid(), getppid(), argc);
	for (int i = 0; i < argc; i++) {
		printf("  argv[%d] = %s\n", i, argv[i]);
	}
	const char *t = getenv("FIRE");
	printf("  FIRE=%s\n", t ? t : "(unset)");
	return 7;
}

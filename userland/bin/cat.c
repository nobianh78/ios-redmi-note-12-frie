/* /bin/cat [FILE...]: copy files (or stdin) to stdout. */
#include "../libc/firelibc.h"

static int
copy(int fd)
{
	char b[4096];
	ssize_t n;
	while ((n = read(fd, b, sizeof(b))) > 0) {
		if (write(1, b, (size_t)n) != n) {
			return -1;
		}
	}
	return (int)n;
}

int
main(int argc, char **argv)
{
	int rc = 0;
	if (argc < 2) {
		return copy(0) < 0;
	}
	for (int i = 1; i < argc; i++) {
		int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
		if (fd < 0 || copy(fd) < 0) {
			dprintf(2, "cat: %s: %s\n", argv[i], strerror(errno));
			rc = 1;
		}
		if (fd > 0) {
			close(fd);
		}
	}
	return rc;
}

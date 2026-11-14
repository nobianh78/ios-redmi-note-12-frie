/* /bin/cp SRC DST: copy one file; DST may be a directory. */
#include "../libc/firelibc.h"

int
main(int argc, char **argv)
{
	if (argc != 3) {
		dprintf(2, "usage: cp SRC DST\n");
		return 2;
	}
	const char *src = argv[1];
	char dst[1024];
	struct stat st;
	if (stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode)) {
		const char *base = src;
		for (const char *p = src; *p; p++) {
			if (*p == '/' && p[1]) {
				base = p + 1;
			}
		}
		snprintf(dst, sizeof(dst), "%s/%s", argv[2], base);
	} else {
		snprintf(dst, sizeof(dst), "%s", argv[2]);
	}
	int in = open(src, O_RDONLY);
	if (in < 0 || fstat(in, &st) < 0) {
		dprintf(2, "cp: %s: %s\n", src, strerror(errno));
		return 1;
	}
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
	if (out < 0) {
		dprintf(2, "cp: %s: %s\n", dst, strerror(errno));
		return 1;
	}
	char b[8192];
	ssize_t n;
	while ((n = read(in, b, sizeof(b))) > 0) {
		if (write(out, b, (size_t)n) != n) {
			dprintf(2, "cp: %s: %s\n", dst, strerror(errno));
			return 1;
		}
	}
	close(in);
	return close(out) < 0 || n < 0;
}

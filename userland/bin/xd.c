/* /bin/xd FILE [OFFSET [LEN]]: hexdump LEN bytes (default 256) from OFFSET (decimal or 0x..). */
#include "../libc/firelibc.h"

int
main(int argc, char **argv)
{
	if (argc < 2) {
		dprintf(2, "usage: xd FILE [OFFSET [LEN]]\n");
		return 2;
	}
	long off = argc > 2 ? strtol(argv[2], NULL, 0) : 0, len = argc > 3 ? strtol(argv[3], NULL, 0) : 256;
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0 || lseek(fd, off, 0) != off) {
		dprintf(2, "xd: %s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	unsigned char b[16];
	for (long at = off; len > 0; at += 16) {
		ssize_t n = read(fd, b, len < 16 ? (size_t)len : 16);
		if (n <= 0) {
			break;
		}
		len -= n;
		printf("%08lx ", at);
		for (int i = 0; i < 16; i++) {
			if (i < n) {
				printf(" %02x", b[i]);
			} else {
				printf("   ");
			}
		}
		printf("  ");
		for (int i = 0; i < n; i++) {
			printf("%c", b[i] >= 32 && b[i] < 127 ? b[i] : '.');
		}
		printf("\n");
	}
	return 0;
}

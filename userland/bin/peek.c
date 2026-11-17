/* /bin/peek ADDR [LEN]: hexdump LEN bytes (default 256) of this process's memory at ADDR. Used to compare
 * the dyld shared cache as the kernel maps it (shared region) against the cache file on disk. */
#include "../libc/firelibc.h"

int
main(int argc, char **argv)
{
	if (argc < 2) {
		dprintf(2, "usage: peek ADDR [LEN]\n");
		return 2;
	}
	unsigned long addr = (unsigned long)strtol(argv[1], NULL, 0), len = argc > 2 ? (unsigned long)strtol(argv[2], NULL, 0) : 256;
	for (unsigned long at = addr; at < addr + len; at += 16) {
		volatile const unsigned char *p = (volatile const unsigned char *)at;
		printf("%016lx ", at);
		for (int i = 0; i < 16; i++) {
			printf(" %02x", p[i]);
		}
		printf("\n");
	}
	return 0;
}

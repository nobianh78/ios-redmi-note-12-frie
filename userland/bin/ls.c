/* /bin/ls [-a] [-l] [PATH...]: sorted listing; -l shows type, mode, links and size. */
#include "../libc/firelibc.h"

static int all, lng;

static int
cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void
entry(const char *dir, const char *name)
{
	if (!lng) {
		printf("%s\n", name);
		return;
	}
	char path[1024];
	struct stat st;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (lstat(dir ? path : name, &st) < 0) {
		printf("?????????? %s\n", name);
		return;
	}
	char m[11];
	int t = st.st_mode & S_IFMT;
	m[0] = t == S_IFDIR ? 'd' : t == 0120000 ? 'l' : t == 0020000 ? 'c' : t == 0060000 ? 'b' : '-';
	for (int i = 0; i < 9; i++) {
		m[1 + i] = (st.st_mode >> (8 - i)) & 1 ? "rwxrwxrwx"[i] : '-';
	}
	m[10] = 0;
	printf("%s %2u %8ld %s\n", m, st.st_nlink, st.st_size, name);
}

static int
list(const char *path)
{
	struct stat st;
	if (stat(path, &st) < 0) {
		dprintf(2, "ls: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (!S_ISDIR(st.st_mode)) {
		entry(NULL, path);
		return 0;
	}
	DIR *d = opendir(path);
	if (!d) {
		dprintf(2, "ls: %s: %s\n", path, strerror(errno));
		return 1;
	}
	char **names = NULL;
	int n = 0, cap = 0;
	for (struct dirent *e; (e = readdir(d));) {
		if (e->d_name[0] == '.' && !all) {
			continue;
		}
		if (n == cap) {
			cap = cap ? cap * 2 : 64;
			names = realloc(names, (size_t)cap * sizeof(*names));
		}
		names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, (size_t)n, sizeof(*names), cmp);
	for (int i = 0; i < n; i++) {
		entry(path, names[i]);
	}
	return 0;
}

int
main(int argc, char **argv)
{
	int i = 1, rc = 0;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		for (const char *f = argv[i] + 1; *f; f++) {
			all |= *f == 'a';
			lng |= *f == 'l';
		}
	}
	if (i == argc) {
		return list(".");
	}
	for (int first = i; i < argc; i++) {
		if (argc - first > 1) {
			printf("%s%s:\n", i > first ? "\n" : "", argv[i]);
		}
		rc |= list(argv[i]);
	}
	return rc;
}

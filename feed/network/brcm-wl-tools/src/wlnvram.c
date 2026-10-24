/* wlnvram: tiny CLI for the wl driver's emulated /dev/nvram (get/set/unset/show/commit). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char *nvram_get(const char *name);
extern int nvram_set(const char *name, const char *value);
extern int nvram_unset(const char *name);
extern int nvram_getall(char *buf, int count);
extern int nvram_commit(void);

int main(int argc, char **argv)
{
	static char all[0x10000];
	char *p, *v;

	if (argc >= 3 && !strcmp(argv[1], "get")) {
		v = nvram_get(argv[2]);
		if (v)
			puts(v);
		return v ? 0 : 1;
	}
	if (argc >= 3 && !strcmp(argv[1], "set")) {
		p = strchr(argv[2], '=');
		if (!p)
			return 1;
		*p = 0;
		return nvram_set(argv[2], p + 1) ? 1 : 0;
	}
	if (argc >= 3 && !strcmp(argv[1], "unset"))
		return nvram_unset(argv[2]) ? 1 : 0;
	if (argc >= 2 && (!strcmp(argv[1], "show") || !strcmp(argv[1], "getall"))) {
		if (nvram_getall(all, sizeof(all)))
			return 1;
		for (p = all; *p; p += strlen(p) + 1)
			puts(p);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "commit"))
		return nvram_commit() ? 1 : 0;
	fprintf(stderr, "usage: wlnvram get NAME | set NAME=VALUE | unset NAME | show | commit\n");
	return 2;
}

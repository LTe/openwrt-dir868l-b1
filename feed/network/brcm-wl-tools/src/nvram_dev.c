/*
 * Minimal libnvram for the Broadcom userland (nas, eapd, wlconf, libshared) on musl.
 * Talks to /dev/nvram as implemented by kmod-brcm-wl-blob, which speaks the SDK protocol:
 *   read(buf = "name")  -> buf receives the offset of the value inside the mmap()ed space
 *   read(buf = "")      -> buf receives all "name=value" pairs
 *   write("name=value") -> set, write("name") -> unset, ioctl(NVRAM_MAGIC) -> commit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#define NVRAM_SPACE	0x10000
#define NVRAM_MAGIC	0x48534C46	/* 'FLSH' */

static int nvram_fd = -1;
static char *nvram_map;

static int nv_open(void)
{
	if (nvram_fd >= 0)
		return 0;
	nvram_fd = open("/dev/nvram", O_RDWR | O_CLOEXEC);
	if (nvram_fd < 0)
		return -1;
	nvram_map = mmap(NULL, NVRAM_SPACE, PROT_READ, MAP_SHARED, nvram_fd, 0);
	if (nvram_map == MAP_FAILED) {
		close(nvram_fd);
		nvram_fd = -1;
		return -1;
	}
	return 0;
}

char *nvram_get(const char *name)
{
	char tmp[128], *buf = tmp, *value = NULL;
	size_t count;
	unsigned long off;

	if (!name || nv_open())
		return NULL;
	count = strlen(name) + 1;
	if (count > sizeof(tmp) && !(buf = malloc(count)))
		return NULL;
	strcpy(buf, name);
	if (read(nvram_fd, buf, count) == (ssize_t)sizeof(off)) {
		memcpy(&off, buf, sizeof(off));
		if (off < NVRAM_SPACE)
			value = nvram_map + off;
	}
	if (buf != tmp)
		free(buf);
	return value;
}

char *nvram_safe_get(const char *name)
{
	char *v = nvram_get(name);
	return v ? v : "";
}

char *nvram_default_get(const char *name)
{
	return NULL;
}

int nvram_getall(char *buf, int count)
{
	if (nv_open() || count < 1)
		return -1;
	buf[0] = 0;
	return read(nvram_fd, buf, count) < 0 ? -1 : 0;
}

static int nv_write(const char *s)
{
	size_t len = strlen(s);
	if (nv_open())
		return -1;
	return write(nvram_fd, s, len) == (ssize_t)len ? 0 : -1;
}

int nvram_set(const char *name, const char *value)
{
	char *s;
	int ret;

	if (!name)
		return -1;
	if (!value)
		return nv_write(name);
	s = malloc(strlen(name) + strlen(value) + 2);
	if (!s)
		return -1;
	sprintf(s, "%s=%s", name, value);
	ret = nv_write(s);
	free(s);
	return ret;
}

int nvram_unset(const char *name)
{
	return name ? nv_write(name) : -1;
}

int nvram_commit(void)
{
	if (nv_open())
		return -1;
	return ioctl(nvram_fd, NVRAM_MAGIC, NULL);
}

int nvram_match(const char *name, const char *match)
{
	const char *v = nvram_get(name);
	return v && !strcmp(v, match);
}

int nvram_invmatch(const char *name, const char *invmatch)
{
	const char *v = nvram_get(name);
	return v && strcmp(v, invmatch);
}

int nvram_get_int(const char *name)
{
	const char *v = nvram_get(name);
	return v ? atoi(v) : 0;
}

void nvram_validate_all(char *prefix, bool restore)
{
}

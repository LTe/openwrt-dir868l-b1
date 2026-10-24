/*
 * Compatibility symbols the prebuilt core imports under new names (see tools/blobsurgery.py):
 * kernel functions that disappeared or changed ABI since 2.6.36, plus stubs for the
 * emf/igs multicast helper modules we do not ship.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/printk.h>
#include <linux/bug.h>
#include <asm/outercache.h>

/* 2.6.36 struct outer_cache_fns: inv_range, clean_range, flush_range, sync */
struct wlblob_outer_cache_fns {
	void (*inv_range)(unsigned long, unsigned long);
	void (*clean_range)(unsigned long, unsigned long);
	void (*flush_range)(unsigned long, unsigned long);
	void (*sync)(void);
};
static void wlblob_outer_nop_range(unsigned long s, unsigned long e) { }
static void wlblob_outer_nop_sync(void) { }
struct wlblob_outer_cache_fns wlblob_outer_cache = {
	wlblob_outer_nop_range, wlblob_outer_nop_range, wlblob_outer_nop_range, wlblob_outer_nop_sync
};

void wlblob_compat_init(void)
{
#ifdef CONFIG_OUTER_CACHE
	if (outer_cache.inv_range)
		wlblob_outer_cache.inv_range = outer_cache.inv_range;
	if (outer_cache.clean_range)
		wlblob_outer_cache.clean_range = outer_cache.clean_range;
	if (outer_cache.flush_range)
		wlblob_outer_cache.flush_range = outer_cache.flush_range;
	if (outer_cache.sync)
		wlblob_outer_cache.sync = outer_cache.sync;
#endif
}

void wlblob_memzero(void *p, size_t n)
{
	memset(p, 0, n);
}

int wlblob_printk(const char *fmt, ...)
{
	va_list ap;
	int r;

	va_start(ap, fmt);
	r = vprintk(fmt, ap);
	va_end(ap);
	return r;
}

void wlblob_warn_slowpath_null(const char *file, int line)
{
	WARN(1, "wl core warning at %s:%d\n", file, line);
}

/* emf/igs (multicast forwarding / IGMP snooping helper modules): not provided */
void *emfc_init(char *inst_id, void *wrapper, void *osh, void *wrapper_ops) { return NULL; }
void emfc_exit(void *emfc) { }
int emfc_cfg_request_process(void *emfc, void *cfg) { return -1; }
int emfc_input(void *emfc, void *sdu, void *ifp, int rt_port) { return 0; }
void *igsc_init(char *inst_id, void *wrapper, void *osh, void *wrapper_ops) { return NULL; }
void igsc_exit(void *igsc) { }
int igsc_interface_rtport_del(void *igsc, void *ifp) { return -1; }
int igsc_sdb_interface_del(void *igsc, void *ifp) { return -1; }

/* ---- extra imports of the FreshTomato 6.37.14.126 build ---- */
#include <linux/bitops.h>
void wlblob_set_bit_le(int nr, void *addr) { set_bit(nr, addr); }
int wlblob_test_and_set_bit_le(int nr, void *addr) { return test_and_set_bit(nr, addr); }
int wlblob_test_and_clear_bit_le(int nr, void *addr) { return test_and_clear_bit(nr, addr); }
/* dual-STA helper module hooks: not present on OpenWrt */
int dpsta_register(int unit, void *psta) { return 0; }

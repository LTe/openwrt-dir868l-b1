/*
 * MMIO record/replay for the re-linked Broadcom wl core.
 *
 * Record (on the router): every register access of the core is reported here by the thunks that
 * tools/blobtrace.py plants in the core (wltrace_emulate decodes and performs the original
 * ldr/str), accesses of the compiled glue/shared code arrive through R_REG/W_REG, PCI config
 * space through the OSL.  Accesses inside a registered window (the ioremapped BAR) are appended
 * to a ring buffer readable from debugfs: /sys/kernel/debug/wltrace/trace (header + records).
 *
 * Replay (in QEMU, no radio): the same module with replay=1 loads brcm/wl-trace.bin and answers
 * window reads with the recorded values (first unconsumed record for the same address and width,
 * searched a bounded distance ahead of the cursor; otherwise the last value seen at that address),
 * skips window writes and counts how far the driver diverges from the recording.
 */
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/seq_file.h>
#include "wltrace.h"

#define NWIN		4
#define RING_RECS	(2u << 20)	/* 32 MB */
#define SCAN_AHEAD	8192
#define WIN_WORDS	(64 * 1024 / 4)

int wltrace_replay;
module_param_named(replay, wltrace_replay, int, 0);
MODULE_PARM_DESC(replay, "answer register reads from brcm/wl-trace.bin instead of hardware");
static int wltrace_enable = 1;
module_param_named(trace, wltrace_enable, int, 0644);
MODULE_PARM_DESC(trace, "record register accesses (default on)");

static struct { void __iomem *base; unsigned long len; } wins[NWIN];
static struct wltrace_rec *ring;
static atomic_t ring_head = ATOMIC_INIT(0);
static atomic_t ring_dropped = ATOMIC_INIT(0);
static u16 rec_vendor, rec_device;

/* replay state */
static const struct firmware *rep_fw;
static const struct wltrace_rec *rep;
static u32 rep_n, rep_cursor;
static unsigned long *rep_consumed;
static u32 (*rep_last)[WIN_WORDS];		/* last value per window word */
static struct { u32 reads, hits, misses, writes, wmatch, wmiss, cfg_hits, cfg_miss; u32 first_miss_site, first_miss_addr; } rstat;

static int find_win(unsigned long addr)
{
	int i;
	for (i = 0; i < NWIN; i++)
		if (wins[i].base && addr - (unsigned long)wins[i].base < wins[i].len)
			return i;
	return -1;
}

static void record(u32 site, u32 addr, u32 value, u16 flags, u8 width, u8 win)
{
	u32 h;
	if (!ring || !wltrace_enable)
		return;
	h = atomic_fetch_add(1, &ring_head);
	if (h >= RING_RECS) {
		atomic_inc(&ring_dropped);
		return;
	}
	ring[h] = (struct wltrace_rec){ .site = site, .addr = addr, .value = value, .flags = flags,
					.width = width, .win = win };
}

/* ---- replay lookups ---- */
static u32 replay_read(u32 site, u32 off, u8 width, int win)
{
	u32 i, end = min(rep_cursor + SCAN_AHEAD, rep_n);
	rstat.reads++;
	for (i = rep_cursor; i < end; i++) {
		const struct wltrace_rec *r = &rep[i];
		if (test_bit(i, rep_consumed) || (r->flags & (WLT_WRITE | WLT_CFG | WLT_IRQ | WLT_MARK)))
			continue;
		if (r->win == win && r->addr == off && r->width == width) {
			set_bit(i, rep_consumed);
			while (rep_cursor < rep_n && test_bit(rep_cursor, rep_consumed))
				rep_cursor++;
			rstat.hits++;
			rep_last[win][off / 4] = r->value;
			return r->value;
		}
	}
	rstat.misses++;
	if (!rstat.first_miss_site) {
		rstat.first_miss_site = site; rstat.first_miss_addr = off;
	}
	record(site, off, rep_last[win][off / 4], WLT_MISS, width, win);
	return rep_last[win][off / 4];
}

static void replay_write(u32 site, u32 off, u8 width, u32 value, int win)
{
	u32 i, end = min(rep_cursor + SCAN_AHEAD, rep_n);
	rstat.writes++;
	rep_last[win][off / 4] = value;
	for (i = rep_cursor; i < end; i++) {
		const struct wltrace_rec *r = &rep[i];
		if (test_bit(i, rep_consumed) || !(r->flags & WLT_WRITE) || (r->flags & WLT_CFG))
			continue;
		if (r->win == win && r->addr == off && r->width == width && r->value == value) {
			set_bit(i, rep_consumed);
			while (rep_cursor < rep_n && test_bit(rep_cursor, rep_consumed))
				rep_cursor++;
			rstat.wmatch++;
			return;
		}
	}
	rstat.wmiss++;
	record(site, off, value, WLT_WRITE | WLT_MISS, width, win);
}

/* ---- the access path shared by core thunks and glue macros ---- */
static u32 do_load(u32 site, unsigned long addr, int width, int sign, u16 flags)
{
	int win = find_win(addr);
	u32 v;

	if (win >= 0 && wltrace_replay) {
		v = replay_read(site, addr - (unsigned long)wins[win].base, width, win);
	} else {
		switch (width) {
		case 1: v = *(volatile u8 *)addr; break;
		case 2: v = *(volatile u16 *)addr; break;
		default: v = *(volatile u32 *)addr; break;
		}
		if (win >= 0)
			record(site, addr - (unsigned long)wins[win].base, v, flags, width, win);
	}
	if (sign)
		v = width == 1 ? (u32)(s32)(s8)v : (u32)(s32)(s16)v;
	return v;
}

static void do_store(u32 site, unsigned long addr, int width, u32 v, u16 flags)
{
	int win = find_win(addr);

	if (win >= 0 && wltrace_replay) {
		replay_write(site, addr - (unsigned long)wins[win].base, width, v, win);
		return;
	}
	switch (width) {
	case 1: *(volatile u8 *)addr = v; break;
	case 2: *(volatile u16 *)addr = v; break;
	default: *(volatile u32 *)addr = v; break;
	}
	if (win >= 0)
		record(site, addr - (unsigned long)wins[win].base, v, flags | WLT_WRITE, width, win);
}

/*
 * Called from the thunks: fr = {cpsr, r0..r12, lr} as pushed, sp at the time of the original
 * instruction is fr + 15 words.  insn is the original ldr/str encoding (ARM A1 single data
 * transfer or extra load/store, pre-filtered by the tool: no pc, no sp base, no ldm/ldrd).
 */
static inline u32 get_reg(u32 *fr, int r)
{
	if (r < 13) return fr[1 + r];
	if (r == 13) return (u32)(fr + 15);
	return fr[14];
}
static inline void set_reg(u32 *fr, int r, u32 v)
{
	if (r < 13) fr[1 + r] = v;
	else if (r == 14) fr[14] = v;
}

void noinline wltrace_emulate(u32 *fr, u32 insn)
{
	u32 site = (u32)__builtin_return_address(0);
	u32 Rn = (insn >> 16) & 15, Rt = (insn >> 12) & 15;
	u32 P = (insn >> 24) & 1, U = (insn >> 23) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
	u32 off, base, addr, wb, val;
	int width, sign = 0;

	if (((insn >> 25) & 6) == 2) {			/* 01x: ldr/str/ldrb/strb */
		width = (insn & (1 << 22)) ? 1 : 4;
		if (insn & (1 << 25)) {			/* register offset, optionally shifted */
			u32 v = get_reg(fr, insn & 15), sh = (insn >> 7) & 31;
			switch ((insn >> 5) & 3) {
			case 0: off = v << sh; break;
			case 1: off = sh ? v >> sh : 0; break;
			case 2: off = sh ? (u32)((s32)v >> sh) : (u32)((s32)v >> 31); break;
			default: off = sh ? ((v >> sh) | (v << (32 - sh))) : (((fr[0] >> 29) & 1) << 31) | (v >> 1); break;
			}
		} else {
			off = insn & 0xfff;
		}
	} else {					/* extra: ldrh/strh/ldrsb/ldrsh */
		u32 op2 = (insn >> 5) & 3;
		off = (insn & (1 << 22)) ? (((insn >> 8) & 15) << 4) | (insn & 15) : get_reg(fr, insn & 15);
		width = op2 == 2 ? 1 : 2;
		sign = op2 >= 2;
	}
	base = get_reg(fr, Rn);
	wb = U ? base + off : base - off;
	addr = P ? wb : base;
	val = get_reg(fr, Rt);
	if (!P || W)
		set_reg(fr, Rn, wb);
	if (L)
		set_reg(fr, Rt, do_load(site, addr, width, sign, 0));
	else
		do_store(site, addr, width, val, 0);
}

u32 wltrace_glue_read(const volatile void __iomem *r, int width)
{
	return do_load((u32)__builtin_return_address(0), (unsigned long)r, width, 0, WLT_GLUE);
}
void wltrace_glue_write(volatile void __iomem *r, int width, u32 v)
{
	do_store((u32)__builtin_return_address(0), (unsigned long)r, width, v, WLT_GLUE);
}

u32 wltrace_cfg_read(u32 off, u32 real)
{
	u32 site = (u32)__builtin_return_address(0);
	if (wltrace_replay) {
		u32 i, end = min(rep_cursor + SCAN_AHEAD, rep_n);
		for (i = rep_cursor; i < end; i++) {
			const struct wltrace_rec *r = &rep[i];
			if (!test_bit(i, rep_consumed) && (r->flags & (WLT_CFG | WLT_WRITE)) == WLT_CFG && r->addr == off) {
				set_bit(i, rep_consumed); rstat.cfg_hits++;
				return r->value;
			}
		}
		rstat.cfg_miss++;
		return 0xffffffff;
	}
	record(site, off, real, WLT_CFG, 4, 0);
	return real;
}
int wltrace_cfg_write(u32 off, u32 val)
{
	record((u32)__builtin_return_address(0), off, val, WLT_CFG | WLT_WRITE, 4, 0);
	return wltrace_replay;
}

void wltrace_mark(u16 flags, u32 value)
{
	record(0, 0, value, flags, 0, 0);
}

void wltrace_set_ids(u16 vendor, u16 device)
{
	if (!wltrace_replay) {
		rec_vendor = vendor; rec_device = device;
	}
}

int wltrace_add_window(void __iomem *base, unsigned long len)
{
	int i;
	for (i = 0; i < NWIN; i++)
		if (!wins[i].base) {
			wins[i].base = base; wins[i].len = len;
			return i;
		}
	return -1;
}
void wltrace_del_window(void __iomem *base)
{
	int i;
	for (i = 0; i < NWIN; i++)
		if (wins[i].base == base)
			wins[i].base = NULL;
}

int wltrace_replay_load(struct device *dev, u16 *vendor, u16 *device)
{
	const struct wltrace_hdr *h;
	int err;

	if (rep)
		goto done;
	err = request_firmware(&rep_fw, "brcm/wl-trace.bin", dev);
	if (err) {
		pr_err("wltrace: brcm/wl-trace.bin: %d\n", err);
		return err;
	}
	h = (const void *)rep_fw->data;
	if (rep_fw->size < sizeof(*h) || h->magic != WLT_MAGIC) {
		pr_err("wltrace: bad trace file\n");
		return -EINVAL;
	}
	rep = (const void *)(rep_fw->data + sizeof(*h));
	rep_n = min_t(u32, h->nrec, (rep_fw->size - sizeof(*h)) / sizeof(struct wltrace_rec));
	rep_consumed = vzalloc(BITS_TO_LONGS(rep_n + 1) * sizeof(long));
	rep_last = vzalloc(sizeof(*rep_last) * NWIN);
	if (!rep_consumed || !rep_last)
		return -ENOMEM;
	rec_vendor = h->vendor; rec_device = h->device;
	pr_info("wltrace: replaying %u records for %04x:%04x\n", rep_n, rec_vendor, rec_device);
done:
	if (vendor) *vendor = rec_vendor;
	if (device) *device = rec_device;
	return 0;
}

/* ---- debugfs ---- */
static ssize_t trace_read(struct file *f, char __user *buf, size_t len, loff_t *pos)
{
	struct wltrace_hdr h = { .magic = WLT_MAGIC, .version = 1, .vendor = rec_vendor, .device = rec_device };
	u32 n = min_t(u32, atomic_read(&ring_head), RING_RECS);
	size_t total, i;

	h.nrec = n;
	for (i = 0; i < NWIN; i++)
		h.winlen[i] = wins[i].base ? wins[i].len : 0;
	total = sizeof(h) + (size_t)n * sizeof(struct wltrace_rec);
	if (*pos >= total)
		return 0;
	len = min(len, (size_t)(total - *pos));
	if (*pos < sizeof(h)) {
		size_t c = min(len, (size_t)(sizeof(h) - *pos));
		if (copy_to_user(buf, (char *)&h + *pos, c))
			return -EFAULT;
		*pos += c;
		return c;
	}
	if (copy_to_user(buf, (char *)ring + (*pos - sizeof(h)), len))
		return -EFAULT;
	*pos += len;
	return len;
}
static ssize_t trace_write(struct file *f, const char __user *buf, size_t len, loff_t *pos)
{
	char cmd[32] = {};
	if (copy_from_user(cmd, buf, min(len, sizeof(cmd) - 1)))
		return -EFAULT;
	if (!strncmp(cmd, "reset", 5)) {
		atomic_set(&ring_head, 0); atomic_set(&ring_dropped, 0);
	} else if (!strncmp(cmd, "mark", 4)) {
		wltrace_mark(WLT_MARK, simple_strtoul(cmd + 4, NULL, 0));
	}
	return len;
}
static const struct file_operations trace_fops = { .read = trace_read, .write = trace_write, .llseek = default_llseek };

static int stats_show(struct seq_file *m, void *v)
{
	int i;
	seq_printf(m, "records %u dropped %u enable %d replay %d\n", min_t(u32, atomic_read(&ring_head), RING_RECS),
		   atomic_read(&ring_dropped), wltrace_enable, wltrace_replay);
	for (i = 0; i < NWIN; i++)
		if (wins[i].base)
			seq_printf(m, "win%d %px len %lu\n", i, wins[i].base, wins[i].len);
	if (wltrace_replay)
		seq_printf(m, "replay: %u/%u consumed, reads %u hits %u misses %u (first miss site %08x addr %x), "
			   "writes %u matched %u unmatched %u, cfg hits %u misses %u\n", rep_cursor, rep_n,
			   rstat.reads, rstat.hits, rstat.misses, rstat.first_miss_site, rstat.first_miss_addr,
			   rstat.writes, rstat.wmatch, rstat.wmiss, rstat.cfg_hits, rstat.cfg_miss);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static struct dentry *dbg;

int wltrace_init(void)
{
	ring = vmalloc(RING_RECS * sizeof(*ring));
	if (!ring)
		return -ENOMEM;
	dbg = debugfs_create_dir("wltrace", NULL);
	debugfs_create_file("trace", 0600, dbg, NULL, &trace_fops);
	debugfs_create_file("stats", 0444, dbg, NULL, &stats_fops);
	pr_info("wltrace: %s, ring %u records\n", wltrace_replay ? "replay" : "record", RING_RECS);
	return 0;
}

void wltrace_exit(void)
{
	debugfs_remove_recursive(dbg);
	vfree(ring); ring = NULL;
	vfree(rep_consumed); vfree(rep_last);
	if (rep_fw)
		release_firmware(rep_fw);
}

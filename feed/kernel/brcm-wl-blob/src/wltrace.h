/* MMIO record/replay for the re-linked wl core (tools/blobtrace.py instruments the core, this is
 * the runtime side).  Built only with WLBLOB_TRACE=1. */
#ifndef _wltrace_h_
#define _wltrace_h_
#include <linux/types.h>

struct wltrace_rec {
	u32 site;	/* kernel VA of the thunk (core) or of the caller (glue/OSL); 0 for markers */
	u32 addr;	/* offset inside the window, or PCI config offset */
	u32 value;
	u16 flags;
	u8 width;	/* 1, 2, 4 */
	u8 win;		/* window index */
} __packed;

#define WLT_WRITE	0x0001
#define WLT_SIGNED	0x0002
#define WLT_CFG		0x0004	/* PCI configuration space */
#define WLT_IRQ		0x0008	/* marker: interrupt handler entered */
#define WLT_GLUE	0x0010	/* access from compiled code (R_REG/W_REG), not from the core */
#define WLT_MISS	0x0020	/* replay: no recorded value was available */
#define WLT_MARK	0x0040	/* marker written through debugfs (phase boundaries) */

struct wltrace_hdr {
	u32 magic;	/* "WLTR" */
	u32 version;
	u32 nrec;
	u16 vendor, device;
	u32 winlen[4];
};
#define WLT_MAGIC	0x52544c57

struct device;
int wltrace_init(void);
void wltrace_exit(void);
void wltrace_set_ids(u16 vendor, u16 device);
int wltrace_add_window(void __iomem *base, unsigned long len);
void wltrace_del_window(void __iomem *base);
void wltrace_mark(u16 flags, u32 value);
u32 wltrace_glue_read(const volatile void __iomem *r, int width);
void wltrace_glue_write(volatile void __iomem *r, int width, u32 v);
u32 wltrace_cfg_read(u32 off, u32 real);		/* records (or replays) a config space read */
int wltrace_cfg_write(u32 off, u32 val);		/* returns 1 when replaying (skip the real write) */
int wltrace_replay_load(struct device *dev, u16 *vendor, u16 *device);
extern int wltrace_replay;

#endif

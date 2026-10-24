/*
 * Linux OS layer for the re-linked Broadcom wl core (wl_ap.ko from D-Link DIR-868L B1,
 * SDK 6.37.14.56, built against kernel 2.6.36.4brcmarm) running on a modern kernel.
 *
 * The prebuilt core has the 2.6.36 Broadcom sk_buff field offsets inlined (PKTDATA, PKTLEN,
 * PKTTAG, PKTNEXT, ...). We therefore never hand it a real sk_buff: every packet is a
 * "shadow" struct wl_skb with exactly that layout, pointing at the data of a real sk_buff.
 * osl_pkt_frmnative()/osl_pkt_tonative() convert at the boundary.
 */
#ifndef _linux_osl_h_
#define _linux_osl_h_

#include <typedefs.h>
#include <linuxver.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/io.h>
#include <linux/skbuff.h>
#include <linux/pci.h>
#include <asm/outercache.h>

/* ---- the shadow packet: layout of struct sk_buff in the D-Link 2.6.36.4brcmarm kernel ---- */
struct wl_skb {
	struct wl_skb		*next;			/*   0 */
	struct wl_skb		*prev;			/*   4 */
	unsigned char		pktc_cb[8];		/*   8  PKTC chain node */
	unsigned char		ctf_pppoe_cb[8];	/*  16 */
	u64			tstamp;			/*  24 */
	struct sk_buff		*native;		/*  32  (was: sk)  back pointer to the real skb */
	struct net_device	*dev;			/*  36 */
	char			cb[48] __aligned(8);	/*  40  wlc_pkttag_t */
	unsigned long		_skb_refdst;		/*  88 */
	void			*sp;			/*  92 */
	void			*ctfpool;		/*  96 */
	u32			pktc_flags;		/* 100 */
	void			*ctf_ipc_txif;		/* 104 */
	u32			napt_idx;		/* 108 */
	u32			napt_flags;		/* 112 */
#ifdef WLSKB_LAYOUT_FT
	u32			ft_ctf_extra;		/* 116  FreshTomato 2.6.36 has one more word here */
#endif
	unsigned int		len;			/* 116  (FT: 120) */
	unsigned int		data_len;		/* 120 */
	u16			mac_len;		/* 124 */
	u16			hdr_len;		/* 126 */
	u32			csum;			/* 128 */
	u32			priority;		/* 132 */
	u8			flags1;			/* 136  local_df:1 cloned:1 ip_summed:2 nohdr:1 nfctinfo:3 */
	u8			flags2;			/* 137  pkt_type:3 fclone:2 ipvs_property:1 peeked:1 nf_trace:1 */
	u16			protocol;		/* 138 */
	void			(*destructor)(struct wl_skb *);	/* 140 */
	int			skb_iif;		/* 144 */
	u32			rxhash;			/* 148 */
	u32			mark;			/* 152 */
	u16			vlan_tci;		/* 156 */
	u16			pad0;			/* 158 */
	unsigned char		*transport_header;	/* 160 */
	unsigned char		*network_header;	/* 164 */
	unsigned char		*mac_header;		/* 168 */
	unsigned char		*tail;			/* 172 */
	unsigned char		*end;			/* 176 */
	unsigned char		*head;			/* 180 */
	unsigned char		*data;			/* 184 */
	unsigned int		truesize;		/* 188 */
	int			users;			/* 192  (FT: 196) */
#ifdef WLSKB_LAYOUT_FT
	u32			pad1[6];		/* 200 .. 224 */
#else
	u32			pad1[4];		/* 196 .. 212 */
#endif
};
#define WLSKB_CLONED		0x02
#define WLSKB_IPSUMMED_MASK	0x0c
#define WLSKB_IPSUMMED_SHIFT	2
#define WLSKB(p)		((struct wl_skb *)(p))

typedef struct wl_skb wl_pkt_t;

/* ---- osl handle ---- */
typedef void (*pktfree_cb_fn_t)(void *ctx, void *pkt, unsigned int status);
typedef struct {
	bool pkttag;
	bool mmbus;
	pktfree_cb_fn_t tx_fn;
	void *tx_ctx;
	void *unused[3];
} osl_pubinfo_t;

#define PKTFREESETCB(osh, _tx_fn, _tx_ctx)	\
	do { ((osl_pubinfo_t*)osh)->tx_fn = _tx_fn; ((osl_pubinfo_t*)osh)->tx_ctx = _tx_ctx; } while (0)

extern osl_t *osl_attach(void *pdev, uint bustype, bool pkttag);
extern void osl_detach(osl_t *osh);
extern struct pci_dev *osl_pci_device(osl_t *osh);
extern uint32 g_assert_type;

#ifdef BCMDBG_ASSERT
#define ASSERT(exp) do { if (unlikely(!(exp))) osl_assert(#exp, __FILE__, __LINE__); } while (0)
extern void osl_assert(const char *exp, const char *file, int line);
#else
#define ASSERT(exp) do {} while (0)
#endif

#define OSL_DELAY(usec)		osl_delay(usec)
extern void osl_delay(uint usec);

#define OSL_PCMCIA_READ_ATTR(osh, offset, buf, size)	do {} while (0)
#define OSL_PCMCIA_WRITE_ATTR(osh, offset, buf, size)	do {} while (0)

#define OSL_PCI_READ_CONFIG(osh, offset, size)		osl_pci_read_config((osh), (offset), (size))
#define OSL_PCI_WRITE_CONFIG(osh, offset, size, val)	osl_pci_write_config((osh), (offset), (size), (val))
extern uint32 osl_pci_read_config(osl_t *osh, uint offset, uint size);
extern void osl_pci_write_config(osl_t *osh, uint offset, uint size, uint val);
#define OSL_PCI_BUS(osh)	osl_pci_bus(osh)
#define OSL_PCI_SLOT(osh)	osl_pci_slot(osh)
#define OSL_PCIE_DOMAIN(osh)	osl_pcie_domain(osh)
#define OSL_PCIE_BUS(osh)	osl_pcie_bus(osh)
extern uint osl_pci_bus(osl_t *osh);
extern uint osl_pci_slot(osl_t *osh);
extern uint osl_pcie_domain(osl_t *osh);
extern uint osl_pcie_bus(osl_t *osh);

#define BUS_SWAP32(v)		(v)

#define MALLOC(osh, size)	osl_malloc((osh), (size))
#define MFREE(osh, addr, size)	osl_mfree((osh), (addr), (size))
#define MALLOCED(osh)		osl_malloced((osh))
#define MALLOC_FAILED(osh)	osl_malloc_failed((osh))
extern void *osl_malloc(osl_t *osh, uint size);
extern void osl_mfree(osl_t *osh, void *addr, uint size);
extern uint osl_malloced(osl_t *osh);
extern uint osl_malloc_failed(osl_t *osh);
#define NATIVE_MALLOC(osh, size)	kmalloc(size, GFP_ATOMIC)
#define NATIVE_MFREE(osh, addr, size)	kfree(addr)

#define DMA_CONSISTENT_ALIGN	osl_dma_consistent_align()
#define DMA_ALLOC_CONSISTENT(osh, size, align, tot, pap, dmah) \
	osl_dma_alloc_consistent((osh), (size), (align), (tot), (pap))
#define DMA_FREE_CONSISTENT(osh, va, size, pa, dmah) \
	osl_dma_free_consistent((osh), (void*)(va), (size), (pa))
#define DMA_ALLOC_CONSISTENT_FORCE32	DMA_ALLOC_CONSISTENT
#define DMA_FREE_CONSISTENT_FORCE32	DMA_FREE_CONSISTENT
extern uint osl_dma_consistent_align(void);
extern void *osl_dma_alloc_consistent(osl_t *osh, uint size, uint16 align, uint *tot, ulong *pap);
extern void osl_dma_free_consistent(osl_t *osh, void *va, uint size, ulong pa);

#define DMA_TX	1
#define DMA_RX	2
#define DMA_MAP(osh, va, size, direction, p, dmah)	osl_dma_map((osh), (va), (size), (direction), (p), (dmah))
#define DMA_UNMAP(osh, pa, size, direction, p, dmah)	osl_dma_unmap((osh), (pa), (size), (direction))
extern uint osl_dma_map(osl_t *osh, void *va, uint size, int direction, void *p, hnddma_seg_map_t *txp_dmah);
extern void osl_dma_unmap(osl_t *osh, uint pa, uint size, int direction);
#define OSL_DMADDRWIDTH(osh, addrwidth)	do {} while (0)

#define SELECT_BUS_WRITE(osh, mmap_op, bus_op)	mmap_op
#define SELECT_BUS_READ(osh, mmap_op, bus_op)	mmap_op

#define OSL_ERROR(bcmerror)	osl_error(bcmerror)
extern int osl_error(int bcmerror);

#define PKTBUFSZ	2048

#define OSL_SYSUPTIME()		((uint32)jiffies_to_msecs(jiffies))
#define printf(fmt, args...)	printk(fmt , ## args)
#define bcopy(src, dst, len)	memcpy((dst), (src), (len))
#define bcmp(b1, b2, len)	memcmp((b1), (b2), (len))
#define bzero(b, len)		memset((b), '\0', (len))

/* register access; the core does an outer_sync() after every write (PL310 WAR), do the same */
static inline void wl_outer_sync(void)
{
#if defined(CONFIG_OUTER_CACHE) && defined(CONFIG_OUTER_CACHE_SYNC)
	if (outer_cache.sync)
		outer_cache.sync();
#endif
}
#define R_REG(osh, r) ({ \
	__typeof(*(r)) __osl_v; \
	switch (sizeof(*(r))) { \
	case sizeof(uint8):  __osl_v = readb((volatile uint8*)(r)); break; \
	case sizeof(uint16): __osl_v = readw((volatile uint16*)(r)); break; \
	default:             __osl_v = readl((volatile uint32*)(r)); break; \
	} \
	__osl_v; })
#define W_REG(osh, r, v) do { \
	switch (sizeof(*(r))) { \
	case sizeof(uint8):  writeb((uint8)(v), (volatile uint8*)(r)); break; \
	case sizeof(uint16): writew((uint16)(v), (volatile uint16*)(r)); break; \
	default:             writel((uint32)(v), (volatile uint32*)(r)); break; \
	} \
	wl_outer_sync(); } while (0)
#define AND_REG(osh, r, v)	W_REG(osh, (r), R_REG(osh, r) & (v))
#define OR_REG(osh, r, v)	W_REG(osh, (r), R_REG(osh, r) | (v))

#define OSL_UNCACHED(va)	((void *)va)
#define OSL_CACHED(va)		((void *)va)
#define OSL_CACHE_FLUSH(va, len)
#define OSL_PREF_RANGE_LD(va, sz)
#define OSL_PREF_RANGE_ST(va, sz)
#define OSL_GETCYCLES(x)	((x) = 0)
#define BUSPROBE(val, addr)	({ (val) = R_REG(NULL, (addr)); 0; })
#define REG_MAP(pa, size)	ioremap((unsigned long)(pa), (unsigned long)(size))
#define REG_UNMAP(va)		iounmap((va))
#define R_SM(r)			*(r)
#define W_SM(r, v)		(*(r) = (v))
#define BZERO_SM(r, len)	memset((r), '\0', (len))

/* ---- packets ---- */
#define PKTGET(osh, len, send)		osl_pktget((osh), (len))
#define PKTDUP(osh, skb)		osl_pktdup((osh), (skb))
#define PKTFREE(osh, skb, send)		osl_pktfree((osh), (skb), (send))
#define PKTLIST_DUMP(osh, buf)
#define PKTDBG_TRACE(osh, pkt, bit)
#define PKTDATA(osh, skb)		(WLSKB(skb)->data)
#define PKTLEN(osh, skb)		(WLSKB(skb)->len)
#define PKTHEADROOM(osh, skb)		(PKTDATA(osh, skb) - WLSKB(skb)->head)
#define PKTEXPHEADROOM(osh, skb, b)	osl_pktexpheadroom((osh), (skb), (b))
#define PKTTAILROOM(osh, skb)		(WLSKB(skb)->data_len ? 0 : (uint)(WLSKB(skb)->end - WLSKB(skb)->tail))
#define PKTNEXT(osh, skb)		(WLSKB(skb)->next)
#define PKTSETNEXT(osh, skb, x)		(WLSKB(skb)->next = WLSKB(x))
#define PKTSETLEN(osh, skb, len)	osl_pktsetlen((osh), (skb), (len))
#define PKTPUSH(osh, skb, bytes)	osl_pktpush((osh), (skb), (bytes))
#define PKTPULL(osh, skb, bytes)	osl_pktpull((osh), (skb), (bytes))
#define PKTTAG(skb)			((void*)(WLSKB(skb)->cb))
#define PKTSETPOOL(osh, skb, x, y)	do {} while (0)
#define PKTPOOL(osh, skb)		FALSE
#define PKTSHRINK(osh, m)		(m)
#define PKTFRMNATIVE(osh, skb)		osl_pkt_frmnative((osl_t *)(osh), (struct sk_buff*)(skb))
#define PKTTONATIVE(osh, pkt)		osl_pkt_tonative((osl_t *)(osh), (pkt))
#define PKTLINK(skb)			(WLSKB(skb)->prev)
#define PKTSETLINK(skb, x)		(WLSKB(skb)->prev = WLSKB(x))
#define PKTPRIO(skb)			(WLSKB(skb)->priority)
#define PKTSETPRIO(skb, x)		(WLSKB(skb)->priority = (x))
#define PKTSUMNEEDED(skb)		(((WLSKB(skb)->flags1 & WLSKB_IPSUMMED_MASK) >> WLSKB_IPSUMMED_SHIFT) == 3)
#define PKTSETSUMGOOD(skb, x)		(WLSKB(skb)->flags1 = (WLSKB(skb)->flags1 & ~WLSKB_IPSUMMED_MASK) | \
						(((x) ? 1 : 0) << WLSKB_IPSUMMED_SHIFT))
#define PKTSHARED(skb)			(WLSKB(skb)->flags1 & WLSKB_CLONED)
#define PKTALLOCED(osh)			osl_pktalloced(osh)
#define PKTMARK(p)			(WLSKB(p)->mark)
#define PKTSETMARK(p, m)		(WLSKB(p)->mark = (m))
#define PKTSETSKIPCT(osh, skb)
#define PKTCLRSKIPCT(osh, skb)
#define PKTSKIPCT(osh, skb)
#define CTF_MARK(m)			0
#define PKTISFAAUX(skb)			(FALSE)
#define PKTISFABRIDGED(skb)		(FALSE)
#define PKTISFAFREED(skb)		(FALSE)
#define PKTCLRFAAUX(skb)
#define PKTSETFAFREED(skb)
#define PKTCLRFAFREED(skb)
#define PKTSETCTFIPCTXIF(skb, ifp)
#define PKTSETTOBR(osh, skb)
#define PKTCLRTOBR(osh, skb)
#define PKTISTOBR(skb)			FALSE
/* chaining flags live in pktc_flags, same bits as the 2.6.36 build the core comes from */
#define CHAINED				(1 << 3)
#define PKTSETCHAINED(osh, skb)		(WLSKB(skb)->pktc_flags |= CHAINED)
#define PKTCLRCHAINED(osh, skb)		(WLSKB(skb)->pktc_flags &= (~CHAINED))
#define PKTISCHAINED(skb)		(WLSKB(skb)->pktc_flags & CHAINED)

extern void *osl_pktget(osl_t *osh, uint len);
extern void *osl_pktdup(osl_t *osh, void *skb);
extern void osl_pktfree(osl_t *osh, void *skb, bool send);
extern uint osl_pktalloced(osl_t *osh);
extern void *osl_pkt_frmnative(osl_t *osh, struct sk_buff *skb);
extern struct sk_buff *osl_pkt_tonative(osl_t *osh, void *pkt);
extern void osl_pktsetlen(osl_t *osh, void *skb, uint len);
extern uchar *osl_pktpush(osl_t *osh, void *skb, int bytes);
extern uchar *osl_pktpull(osl_t *osh, void *skb, int bytes);
extern void *osl_pktexpheadroom(osl_t *osh, void *skb, int bytes);

#ifdef PKTC
struct chain_node {
	struct wl_skb	*link;
	unsigned int	flags:3, pkts:9, bytes:20;
};
#define CHAIN_NODE(skb)		((struct chain_node*)(WLSKB(skb)->pktc_cb))
#define PKTCSETATTR(s, f, p, b)	({CHAIN_NODE(s)->flags = (f); CHAIN_NODE(s)->pkts = (p); CHAIN_NODE(s)->bytes = (b);})
#define PKTCCLRATTR(s)		({CHAIN_NODE(s)->flags = CHAIN_NODE(s)->pkts = CHAIN_NODE(s)->bytes = 0;})
#define PKTCGETATTR(s)		(CHAIN_NODE(s)->flags << 29 | CHAIN_NODE(s)->pkts << 20 | CHAIN_NODE(s)->bytes)
#define PKTCCNT(skb)		(CHAIN_NODE(skb)->pkts)
#define PKTCLEN(skb)		(CHAIN_NODE(skb)->bytes)
#define PKTCGETFLAGS(skb)	(CHAIN_NODE(skb)->flags)
#define PKTCSETFLAGS(skb, f)	(CHAIN_NODE(skb)->flags = (f))
#define PKTCCLRFLAGS(skb)	(CHAIN_NODE(skb)->flags = 0)
#define PKTCFLAGS(skb)		(CHAIN_NODE(skb)->flags)
#define PKTCSETCNT(skb, c)	(CHAIN_NODE(skb)->pkts = (c))
#define PKTCINCRCNT(skb)	(CHAIN_NODE(skb)->pkts++)
#define PKTCADDCNT(skb, c)	(CHAIN_NODE(skb)->pkts += (c))
#define PKTCSETLEN(skb, l)	(CHAIN_NODE(skb)->bytes = (l))
#define PKTCADDLEN(skb, l)	(CHAIN_NODE(skb)->bytes += (l))
#define PKTCSETFLAG(skb, fb)	(CHAIN_NODE(skb)->flags |= (fb))
#define PKTCCLRFLAG(skb, fb)	(CHAIN_NODE(skb)->flags &= ~(fb))
#define PKTCLINK(skb)		(CHAIN_NODE(skb)->link)
#define PKTSETCLINK(skb, x)	(CHAIN_NODE(skb)->link = WLSKB(x))
#define FOREACH_CHAINED_PKT(skb, nskb) \
	for (; (skb) != NULL; (skb) = (nskb)) \
		if ((nskb) = (PKTISCHAINED(skb) ? PKTCLINK(skb) : NULL), PKTSETCLINK((skb), NULL), 1)
#define PKTCFREE(osh, skb, send) \
do { \
	void *nskb; \
	FOREACH_CHAINED_PKT((skb), nskb) { \
		PKTCLRCHAINED((osh), (skb)); \
		PKTCCLRFLAGS((skb)); \
		PKTFREE((osh), (skb), (send)); \
	} \
} while (0)
#define PKTCENQTAIL(h, t, p) \
do { \
	if ((t) == NULL) { (h) = (t) = (p); } else { PKTSETCLINK((t), (p)); (t) = (p); } \
} while (0)
#endif /* PKTC */

#endif /* _linux_osl_h_ */

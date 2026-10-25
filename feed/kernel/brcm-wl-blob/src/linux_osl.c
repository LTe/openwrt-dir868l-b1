/*
 * Linux OS layer for the re-linked Broadcom wl core: memory, DMA, PCI config and the
 * shadow-packet machinery (see include/linux_osl.h).
 */
#include <typedefs.h>
#include <bcmendian.h>
#include <linuxver.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <pcicfg.h>

#define OS_HANDLE_MAGIC		0x1234abcd
#ifdef WLSKB_LAYOUT_FT
#define WLSKB_SIZE		224
#define WLSKB_FT_SHIFT		4
#else
#define WLSKB_SIZE		216
#define WLSKB_FT_SHIFT		0
#endif
#define PCI_CFG_RETRY		10

/* Private metadata follows, and does not change, the blob's skb ABI. */
struct wl_packet {
	struct wl_skb skb;
	struct wl_packet *pool_next;
	unsigned char *pool_data;
	bool poolable;
};
#define AMSDU_HEADER_LEN 192
#define AMSDU_POOL_LIMIT 64

struct osl_info {
	osl_pubinfo_t pub;
	uint magic;
	struct pci_dev *pdev;
	atomic_t malloced;
	atomic_t pktalloced;
	uint failed;
	uint bustype;
	/* coherent allocations (descriptor rings): the SDK built with BCM47XX_CA9 flushes
	 * descriptor cache lines through DMA_MAP(); those buffers are uncached here and live
	 * in the vmalloc area, so DMA_MAP/UNMAP on them must just translate addresses. */
	struct { void *va; dma_addr_t pa; uint size; } coh[32];
	uint ncoh;
	spinlock_t pool_lock;
	struct wl_packet *header_pool;
	uint header_pool_count;
};

uint32 g_assert_type = 0;
static struct kmem_cache *wlskb_cache;
static atomic_t osl_users = ATOMIC_INIT(0);
static bool amsdu_pool;
module_param(amsdu_pool, bool, 0644);
MODULE_PARM_DESC(amsdu_pool, "experimental reuse of internal 192-byte header buffers");
static atomic_t pool_hits = ATOMIC_INIT(0), pool_misses = ATOMIC_INIT(0);
static atomic_t pool_puts = ATOMIC_INIT(0), pool_cached = ATOMIC_INIT(0);
static atomic_t pool_rejects = ATOMIC_INIT(0);

static int amsdu_pool_stats_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "hits=%d misses=%d puts=%d cached=%d rejects=%d\n",
		atomic_read(&pool_hits), atomic_read(&pool_misses), atomic_read(&pool_puts),
		atomic_read(&pool_cached), atomic_read(&pool_rejects));
}
static const struct kernel_param_ops amsdu_pool_stats_ops = { .get = amsdu_pool_stats_get };
module_param_cb(amsdu_pool_stats, &amsdu_pool_stats_ops, NULL, 0400);

bool wlblob_amsdu_pool_test_enabled(void)
{
	return READ_ONCE(amsdu_pool);
}

void osl_assert(const char *exp, const char *file, int line)
{
	const char *basename = strrchr(file, '/');
	printk(KERN_ERR "wl: assertion \"%s\" failed: file \"%s\", line %d\n",
	       exp, basename ? basename + 1 : file, line);
	if (g_assert_type == 1)
		BUG();
	else if (g_assert_type == 2)
		dump_stack();
}

static void wlskb_layout_check(void)
{
	BUILD_BUG_ON(offsetof(struct wl_skb, pktc_cb) != 8);
	BUILD_BUG_ON(offsetof(struct wl_skb, native) != 32);
	BUILD_BUG_ON(offsetof(struct wl_skb, dev) != 36);
	BUILD_BUG_ON(offsetof(struct wl_skb, cb) != 40);
	BUILD_BUG_ON(offsetof(struct wl_skb, pktc_flags) != 100);
	BUILD_BUG_ON(offsetof(struct wl_skb, len) != 116 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, data_len) != 120 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, priority) != 132 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, flags1) != 136 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, protocol) != 138 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, tail) != 172 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, end) != 176 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, head) != 180 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(offsetof(struct wl_skb, data) != 184 + WLSKB_FT_SHIFT);
	BUILD_BUG_ON(sizeof(struct wl_skb) != WLSKB_SIZE);
	BUILD_BUG_ON(offsetof(struct wl_packet, skb) != 0);
}

osl_t *osl_attach(void *pdev, uint bustype, bool pkttag)
{
	osl_t *osh;

	wlskb_layout_check();
	osh = kzalloc(sizeof(osl_t), GFP_KERNEL);
	if (!osh)
		return NULL;
	if (atomic_inc_return(&osl_users) == 1) {
		wlskb_cache = kmem_cache_create("wl_skb", sizeof(struct wl_packet), 0, 0, NULL);
		if (!wlskb_cache) {
			atomic_dec(&osl_users);
			kfree(osh);
			return NULL;
		}
	}
	spin_lock_init(&osh->pool_lock);
	osh->magic = OS_HANDLE_MAGIC;
	osh->pdev = pdev;
	osh->pub.pkttag = pkttag;
	osh->pub.mmbus = TRUE;
	osh->bustype = bustype;
	return osh;
}

void osl_detach(osl_t *osh)
{
	struct wl_packet *entry;
	if (!osh)
		return;
	/* Core callbacks and packet users have already drained at detach. */
	while ((entry = osh->header_pool)) {
		osh->header_pool = entry->pool_next;
		dev_kfree_skb_any(entry->skb.native);
		kmem_cache_free(wlskb_cache, entry);
		atomic_dec(&pool_cached);
	}
	kfree(osh);
	if (atomic_dec_and_test(&osl_users)) {
		kmem_cache_destroy(wlskb_cache);
		wlskb_cache = NULL;
	}
}

struct pci_dev *osl_pci_device(osl_t *osh)
{
	return osh->pdev;
}

/* ---- shadow packets ---- */
static void wlskb_init(osl_t *osh, struct wl_skb *p, struct sk_buff *skb)
{
	p->native = skb;
	p->dev = skb->dev;
	p->head = skb->head;
	p->data = skb->data;
	p->len = skb->len;
	p->tail = skb_tail_pointer(skb);
	p->end = skb_end_pointer(skb);
	p->priority = skb->priority;
	p->protocol = skb->protocol;
	p->mark = skb->mark;
	p->truesize = skb->truesize;
	p->users = 1;
	if (skb_cloned(skb))
		p->flags1 |= WLSKB_CLONED;
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		p->flags1 |= 3 << WLSKB_IPSUMMED_SHIFT;
	atomic_inc(&osh->pktalloced);
}

static struct wl_skb *wlskb_alloc(osl_t *osh, struct sk_buff *skb)
{
	struct wl_skb *p = kmem_cache_zalloc(wlskb_cache, GFP_ATOMIC);
	if (!p)
		return NULL;
	wlskb_init(osh, p, skb);
	return p;
}

static struct wl_skb *wlskb_pool_take(osl_t *osh)
{
	struct wl_packet *entry;
	struct sk_buff *skb;
	unsigned long flags;

	spin_lock_irqsave(&osh->pool_lock, flags);
	entry = osh->header_pool;
	if (entry) {
		osh->header_pool = entry->pool_next;
		osh->header_pool_count--;
		atomic_dec(&pool_cached);
	}
	spin_unlock_irqrestore(&osh->pool_lock, flags);
	if (!entry) {
		atomic_inc(&pool_misses);
		return NULL;
	}
	atomic_inc(&pool_hits);
	skb = entry->skb.native;
	skb->data = entry->pool_data;
	skb->len = 0;
	skb_reset_tail_pointer(skb);
	skb_put(skb, AMSDU_HEADER_LEN);
	skb->priority = 0;
	skb->protocol = 0;
	skb->mark = 0;
	skb->dev = NULL;
	skb->ip_summed = CHECKSUM_NONE;
	skb->next = NULL;
	skb->prev = NULL;
	memset(&entry->skb, 0, sizeof(entry->skb));
	entry->pool_next = NULL;
	wlskb_init(osh, &entry->skb, skb);
	return &entry->skb;
}

static bool wlskb_pool_return(osl_t *osh, struct wl_skb *p)
{
	struct wl_packet *entry = (struct wl_packet *)p;
	struct sk_buff *skb = p->native;
	unsigned long flags;
	bool cached = false;

	if (!entry->poolable || !READ_ONCE(amsdu_pool))
		return false;
	/* Only OSL-owned linear buffers which have not escaped to the stack.
	 * A headroom expansion can share the original data through skb_clone().
	 */
	if (!skb || skb_shared(skb) || skb_cloned(skb) || skb_is_nonlinear(skb) ||
	    skb->sk || skb->destructor) {
		atomic_inc(&pool_rejects);
		return false;
	}
	spin_lock_irqsave(&osh->pool_lock, flags);
	if (osh->header_pool_count < AMSDU_POOL_LIMIT) {
		entry->pool_next = osh->header_pool;
		osh->header_pool = entry;
		osh->header_pool_count++;
		cached = true;
		atomic_inc(&pool_puts);
		atomic_inc(&pool_cached);
	}
	spin_unlock_irqrestore(&osh->pool_lock, flags);
	return cached;
}

/* bring the real skb in sync with what the core did to the shadow */
static struct sk_buff *wlskb_sync(struct wl_skb *p)
{
	struct sk_buff *skb = p->native;

	skb->data = p->data;
	skb->len = p->len;
	skb_reset_tail_pointer(skb);
	skb->tail += p->len;
	skb->priority = p->priority;
	skb->dev = p->dev;
	return skb;
}

void *osl_pkt_frmnative(osl_t *osh, struct sk_buff *skb)
{
	struct wl_skb *head = NULL, *tail = NULL;

	/* linearise and walk a native next-list like the original osl did */
	for (; skb; skb = skb->next) {
		struct wl_skb *p;

		if (skb_is_nonlinear(skb) && skb_linearize(skb))
			continue;
		p = wlskb_alloc(osh, skb);
		if (!p)
			continue;
		if (tail)
			tail->next = p;
		else
			head = p;
		tail = p;
	}
	return head;
}

struct sk_buff *osl_pkt_tonative(osl_t *osh, void *pkt)
{
	struct wl_skb *p = pkt;
	struct sk_buff *head = NULL, *tail = NULL;

	while (p) {
		struct wl_skb *next = p->next;
		struct sk_buff *skb = wlskb_sync(p);

		skb->next = NULL;
		if (tail)
			tail->next = skb;
		else
			head = skb;
		tail = skb;
		kmem_cache_free(wlskb_cache, p);
		atomic_dec(&osh->pktalloced);
		p = next;
	}
	return head;
}

void *osl_pktget(osl_t *osh, uint len)
{
	struct sk_buff *skb;
	struct wl_skb *p;

	if (len == AMSDU_HEADER_LEN && READ_ONCE(amsdu_pool)) {
		p = wlskb_pool_take(osh);
		if (p)
			return p;
	}
	skb = __dev_alloc_skb(len + 32, GFP_ATOMIC);
	if (!skb)
		return NULL;
	/* same alignment games as the original: data 32 byte aligned */
	skb_reserve(skb, PTR_ALIGN(skb->data, 32) - skb->data);
	skb_put(skb, len);
	p = wlskb_alloc(osh, skb);
	if (!p)
		dev_kfree_skb_any(skb);
	else if (len == AMSDU_HEADER_LEN) {
		struct wl_packet *entry = (struct wl_packet *)p;
		entry->poolable = true;
		entry->pool_data = skb->data;
	}
	return p;
}

void osl_pktfree(osl_t *osh, void *pkt, bool send)
{
	struct wl_skb *p = pkt;

	while (p) {
		struct wl_skb *next = p->next;

		if (send && osh->pub.tx_fn)
			osh->pub.tx_fn(osh->pub.tx_ctx, p, 0);
		p->next = NULL;
		if (!wlskb_pool_return(osh, p)) {
			if (p->native)
				dev_kfree_skb_any(p->native);
			kmem_cache_free(wlskb_cache, p);
		}
		atomic_dec(&osh->pktalloced);
		p = next;
	}
}

void *osl_pktdup(osl_t *osh, void *pkt)
{
	struct wl_skb *p = pkt, *dup;
	struct sk_buff *skb;

	/* a real copy, not a clone: the core may rewrite headers of the duplicate */
	skb = skb_copy(wlskb_sync(p), GFP_ATOMIC);
	if (!skb)
		return NULL;
	dup = wlskb_alloc(osh, skb);
	if (!dup) {
		dev_kfree_skb_any(skb);
		return NULL;
	}
	if (osh->pub.pkttag)
		memset(dup->cb, 0, sizeof(dup->cb));
	dup->pktc_flags = 0;
	return dup;
}

uint osl_pktalloced(osl_t *osh)
{
	return atomic_read(&osh->pktalloced);
}

void osl_pktsetlen(osl_t *osh, void *pkt, uint len)
{
	struct wl_skb *p = pkt;

	if (p->data_len)
		return;
	p->len = len;
	p->tail = p->data + len;
}

uchar *osl_pktpush(osl_t *osh, void *pkt, int bytes)
{
	struct wl_skb *p = pkt;

	p->data -= bytes;
	p->len += bytes;
	if (unlikely(p->data < p->head))
		printk(KERN_ERR "wl: pktpush under head (%d bytes, headroom %d)\n", bytes, (int)(p->data + bytes - p->head));
	return p->data;
}

uchar *osl_pktpull(osl_t *osh, void *pkt, int bytes)
{
	struct wl_skb *p = pkt;

	if (unlikely((uint)bytes > p->len))
		return NULL;
	p->len -= bytes;
	p->data += bytes;
	return p->data;
}

void *osl_pktexpheadroom(osl_t *osh, void *pkt, int bytes)
{
	struct wl_skb *p = pkt;
	struct sk_buff *skb = wlskb_sync(p), *nskb;
	struct wl_skb *np;

	nskb = skb_realloc_headroom(skb, bytes);
	if (!nskb)
		return NULL;
	np = wlskb_alloc(osh, nskb);
	if (!np) {
		dev_kfree_skb_any(nskb);
		return NULL;
	}
	memcpy(np->cb, p->cb, sizeof(np->cb));
	memcpy(np->pktc_cb, p->pktc_cb, sizeof(np->pktc_cb));
	np->pktc_flags = p->pktc_flags;
	np->next = p->next;
	np->prev = p->prev;
	p->next = NULL;
	osl_pktfree(osh, p, FALSE);
	return np;
}

/* kernel's skb_push/skb_pull as referenced by the core: operate on shadows */
unsigned char *wlblob_skb_push(void *pkt, unsigned int len)
{
	return osl_pktpush(NULL, pkt, len);
}
unsigned char *wlblob_skb_pull(void *pkt, unsigned int len)
{
	return osl_pktpull(NULL, pkt, len);
}

/* ---- memory ---- */
void *osl_malloc(osl_t *osh, uint size)
{
	void *addr;
	gfp_t flags = (in_interrupt() || irqs_disabled() || in_atomic()) ? GFP_ATOMIC : GFP_KERNEL;

	addr = kmalloc(size, flags | __GFP_NOWARN);
	if (!addr && flags == GFP_KERNEL) {
		addr = vmalloc(size);
		printk(KERN_WARNING "wl: osl_malloc(%u): kmalloc failed, vmalloc fallback %px\n", size, addr);
		dump_stack();
	}
	if (!addr) {
		if (osh)
			osh->failed++;
		printk(KERN_ERR "wl: osl_malloc(%u) failed\n", size);
		return NULL;
	}
	if (osh)
		atomic_add(size, &osh->malloced);
	return addr;
}

void osl_mfree(osl_t *osh, void *addr, uint size)
{
	if (osh)
		atomic_sub(size, &osh->malloced);
	kvfree(addr);
}

uint osl_malloced(osl_t *osh)
{
	return atomic_read(&osh->malloced);
}

uint osl_malloc_failed(osl_t *osh)
{
	return osh->failed;
}

/* ---- DMA ---- */
uint osl_dma_consistent_align(void)
{
	return PAGE_SIZE;
}

void *osl_dma_alloc_consistent(osl_t *osh, uint size, uint16 align_bits, uint *alloced, ulong *pap)
{
	void *va;
	dma_addr_t pa;
	uint16 align = 1 << align_bits;

	if (!ISALIGNED(DMA_CONSISTENT_ALIGN, align))
		size += align;
	*alloced = size;
	/* GFP_ATOMIC would come from the small atomic coherent pool (256 KiB by default),
	 * which two radios worth of descriptor rings exhaust; attach runs in process context */
	va = dma_alloc_coherent(&osh->pdev->dev, size, &pa,
				(in_interrupt() || irqs_disabled() || in_atomic()) ? GFP_ATOMIC : GFP_KERNEL);
	*pap = pa;
	if (!va)
		printk(KERN_ERR "wl: dma_alloc_coherent(%u) failed for %s (mask %llx coherent %llx, atomic=%d)\n",
		       size, pci_name(osh->pdev), (unsigned long long)dma_get_mask(&osh->pdev->dev),
		       (unsigned long long)osh->pdev->dev.coherent_dma_mask,
		       in_interrupt() || irqs_disabled() || in_atomic());
	if (va) {
		if (osh->ncoh < ARRAY_SIZE(osh->coh)) {
			osh->coh[osh->ncoh].va = va;
			osh->coh[osh->ncoh].pa = pa;
			osh->coh[osh->ncoh].size = size;
			osh->ncoh++;
		} else
			printk(KERN_WARNING "wl: too many coherent allocations to track\n");
	}
	return va;
}

void osl_dma_free_consistent(osl_t *osh, void *va, uint size, ulong pa)
{
	uint i;

	for (i = 0; i < osh->ncoh; i++)
		if (osh->coh[i].va == va) {
			osh->coh[i] = osh->coh[--osh->ncoh];
			break;
		}
	dma_free_coherent(&osh->pdev->dev, size, va, (dma_addr_t)pa);
}

static int osl_coh_lookup_va(osl_t *osh, const void *va, dma_addr_t *pa)
{
	uint i;

	for (i = 0; i < osh->ncoh; i++) {
		const char *b = osh->coh[i].va;
		if ((const char *)va >= b && (const char *)va < b + osh->coh[i].size) {
			*pa = osh->coh[i].pa + ((const char *)va - b);
			return 1;
		}
	}
	return 0;
}

static int osl_coh_lookup_pa(osl_t *osh, dma_addr_t pa)
{
	uint i;

	for (i = 0; i < osh->ncoh; i++)
		if (pa >= osh->coh[i].pa && pa < osh->coh[i].pa + osh->coh[i].size)
			return 1;
	return 0;
}

uint osl_dma_map(osl_t *osh, void *va, uint size, int direction, void *p, hnddma_seg_map_t *dmah)
{
	int dir = (direction == DMA_TX) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	dma_addr_t pa;

	if (osl_coh_lookup_va(osh, va, &pa))
		return pa;	/* uncached coherent memory: nothing to flush, just translate */
	pa = dma_map_single(&osh->pdev->dev, va, size, dir);
	if (dma_mapping_error(&osh->pdev->dev, pa)) {
		printk_ratelimited(KERN_ERR "wl: dma_map_single(%px, %u, vmalloc=%d) failed\n",
				   va, size, is_vmalloc_addr(va));
		return 0;
	}
	return pa;
}

void osl_dma_unmap(osl_t *osh, uint pa, uint size, int direction)
{
	int dir = (direction == DMA_TX) ? DMA_TO_DEVICE : DMA_FROM_DEVICE;

	if (osl_coh_lookup_pa(osh, pa))
		return;
	dma_unmap_single(&osh->pdev->dev, (dma_addr_t)pa, size, dir);
}

/* ---- misc ---- */
void osl_delay(uint usec)
{
	while (usec) {
		uint d = min(usec, 1000u);
		udelay(d);
		usec -= d;
	}
}

uint32 osl_pci_read_config(osl_t *osh, uint offset, uint size)
{
	uint val = 0;
	uint retry = PCI_CFG_RETRY;

	do {
		pci_read_config_dword(osh->pdev, offset, &val);
		if (val != 0xffffffff)
			break;
	} while (retry--);
#ifdef WLBLOB_TRACE
	val = wltrace_cfg_read(offset, val);
#endif
	return val;
}

void osl_pci_write_config(osl_t *osh, uint offset, uint size, uint val)
{
	uint retry = PCI_CFG_RETRY;

#ifdef WLBLOB_TRACE
	if (wltrace_cfg_write(offset, val))
		return;
#endif
	do {
		pci_write_config_dword(osh->pdev, offset, val);
		if (offset != PCI_BAR0_WIN)
			break;
		if (osl_pci_read_config(osh, offset, size) == val)
			break;
	} while (retry--);
}

/* The 2.6.36 SDK kernel numbered the two PCIe host bridges as buses 1 and 2 on one
 * domain, and the NVRAM devpaths (pci/1/1, pci/2/1) follow that. Linux 6.x gives each
 * bridge its own domain with bus 1, so fold the domain in to keep the devpaths. */
uint osl_pci_bus(osl_t *osh)
{
	return pci_domain_nr(osh->pdev->bus) + osh->pdev->bus->number;
}

/* Same story for the slot: the SDK kernel enumerated the endpoint behind each root
 * port as device 1, Linux 6.x as device 0. The devpaths are pci/1/1 and pci/2/1. */
uint osl_pci_slot(osl_t *osh)
{
	return PCI_SLOT(osh->pdev->devfn) + 1;
}

uint osl_pcie_domain(osl_t *osh)
{
	return pci_domain_nr(osh->pdev->bus);
}

uint osl_pcie_bus(osl_t *osh)
{
	return osh->pdev->bus->number;
}

int osl_error(int bcmerror)
{
	if (bcmerror > 0)
		bcmerror = 0;
	else if (bcmerror < BCME_LAST)
		bcmerror = BCME_ERROR;
	return bcmerror;	/* bcme errors are negative and distinct, keep them */
}

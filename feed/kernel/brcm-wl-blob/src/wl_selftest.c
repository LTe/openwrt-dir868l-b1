/*
 * Built-in self test of the glue/OSL layer (module parameter selftest=1): exercises the
 * shadow packet machinery and the timer API without any wireless hardware, so that gross
 * bugs in our own code show up in an emulator before touching the router.
 */
#include <typedefs.h>
#include <linuxver.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <linux/pci.h>
#include <linux/skbuff.h>
#include <linux/delay.h>

struct wl_info;
struct wl_timer;
extern struct wl_timer *wl_init_timer(struct wl_info *wl, void (*fn)(void *arg), void *arg, const char *name);
extern void wl_add_timer(struct wl_info *wl, struct wl_timer *t, uint ms, int periodic);
extern bool wl_del_timer(struct wl_info *wl, struct wl_timer *t);
extern void wl_free_timer(struct wl_info *wl, struct wl_timer *t);
extern struct wl_info *wl_selftest_alloc_wl(osl_t *osh);
extern void wl_selftest_free_wl(struct wl_info *wl);

static int st_fail;
#define CHECK(cond, fmt, ...) do { if (!(cond)) { st_fail++; printk(KERN_ERR "wl selftest FAIL: " fmt "\n", ##__VA_ARGS__); } } while (0)

static int timer_hits;
static void st_timer_fn(void *arg) { timer_hits += (int)(long)arg; }

int wl_selftest(struct pci_dev *pdev)
{
	osl_t *osh;
	void *p, *p2, *p3, *q;
	struct sk_buff *skb, *n;
	struct wl_info *wl;
	struct wl_timer *t1, *t2;
	uchar *d;
	struct pktq pq;
	int i;

	st_fail = 0;
	osh = osl_attach(pdev, PCI_BUS, TRUE);
	CHECK(osh != NULL, "osl_attach");
	if (!osh)
		return -1;

	/* 1. PKTGET / PKTDATA / PKTLEN / headroom / tailroom */
	p = PKTGET(osh, 1500, TRUE);
	CHECK(p != NULL, "PKTGET");
	CHECK(PKTLEN(osh, p) == 1500, "PKTLEN %u", PKTLEN(osh, p));
	CHECK(((ulong)PKTDATA(osh, p) & 31) == 0, "PKTDATA alignment %p", PKTDATA(osh, p));
	CHECK(PKTHEADROOM(osh, p) >= 32, "headroom %u", (uint)PKTHEADROOM(osh, p));
	CHECK(PKTTAILROOM(osh, p) >= 0, "tailroom");
	CHECK(PKTALLOCED(osh) == 1, "pktalloced %u", PKTALLOCED(osh));

	/* 2. push/pull/setlen keep the shadow consistent with the native skb */
	d = PKTDATA(osh, p);
	memset(d, 0xaa, 1500);
	CHECK(PKTPUSH(osh, p, 16) == d - 16, "PKTPUSH");
	CHECK(PKTLEN(osh, p) == 1516, "len after push %u", PKTLEN(osh, p));
	CHECK(PKTPULL(osh, p, 16) == d, "PKTPULL");
	PKTSETLEN(osh, p, 100);
	CHECK(PKTLEN(osh, p) == 100, "PKTSETLEN");
	PKTSETPRIO(p, 5);
	skb = PKTTONATIVE(osh, p);
	CHECK(skb != NULL && skb->len == 100 && skb->data == d && skb->priority == 5, "tonative len=%u", skb ? skb->len : 0);
	CHECK(skb_tail_pointer(skb) == skb->data + 100, "tonative tail");
	CHECK(PKTALLOCED(osh) == 0, "pktalloced after tonative %u", PKTALLOCED(osh));

	/* 3. frmnative of a native list -> shadow list, then free the chain */
	n = alloc_skb(200, GFP_KERNEL);
	skb_put(n, 64);
	skb->next = n;
	p = PKTFRMNATIVE(osh, skb);
	CHECK(p != NULL && PKTNEXT(osh, p) != NULL && PKTNEXT(osh, PKTNEXT(osh, p)) == NULL, "frmnative list");
	CHECK(PKTLEN(osh, PKTNEXT(osh, p)) == 64, "frmnative 2nd len");
	CHECK(PKTALLOCED(osh) == 2, "pktalloced list %u", PKTALLOCED(osh));
	PKTFREE(osh, p, FALSE);
	CHECK(PKTALLOCED(osh) == 0, "pktalloced after free %u", PKTALLOCED(osh));

	/* 4. dup, headroom expansion */
	p = PKTGET(osh, 300, TRUE);
	memset(PKTDATA(osh, p), 0x5a, 300);
	p2 = PKTDUP(osh, p);
	CHECK(p2 && PKTLEN(osh, p2) == 300 && !memcmp(PKTDATA(osh, p2), PKTDATA(osh, p), 300), "PKTDUP");
	CHECK(PKTDATA(osh, p2) != PKTDATA(osh, p), "PKTDUP shares data");
	p3 = PKTEXPHEADROOM(osh, p2, 512);
	CHECK(p3 && PKTHEADROOM(osh, p3) >= 512 && PKTLEN(osh, p3) == 300, "PKTEXPHEADROOM");
	CHECK(!memcmp(PKTDATA(osh, p3), PKTDATA(osh, p), 300), "expheadroom data");
	PKTFREE(osh, p, FALSE);
	PKTFREE(osh, p3, FALSE);
	CHECK(PKTALLOCED(osh) == 0, "pktalloced after dup/exp %u", PKTALLOCED(osh));

	/* 5. packet chaining (pktc) and the bcmutils packet queue */
	p = PKTGET(osh, 100, TRUE); p2 = PKTGET(osh, 100, TRUE);
	PKTSETCLINK(p, p2); PKTSETCHAINED(osh, p); PKTCSETCNT(p, 2);
	CHECK(PKTISCHAINED(p) && PKTCLINK(p) == p2 && PKTCCNT(p) == 2, "pktc attrs");
	q = p; { void *nq; i = 0; FOREACH_CHAINED_PKT(q, nq) { i++; PKTCLRCHAINED(osh, q); PKTFREE(osh, q, FALSE); } }
	CHECK(i == 2, "chain walk %d", i);
	pktq_init(&pq, 4, 16);
	for (i = 0; i < 5; i++) {
		p = PKTGET(osh, 64, TRUE);
		PKTSETPRIO(p, i % 4);
		pktq_penq(&pq, i % 4, p);
	}
	CHECK(pktq_mlen(&pq, 0xf) == 5, "pktq len %d", pktq_mlen(&pq, 0xf));
	p = pktq_pdeq(&pq, 1);
	CHECK(p && PKTPRIO(p) == 1, "pktq deq prio");
	PKTFREE(osh, p, FALSE);
	pktq_flush(osh, &pq, TRUE, NULL, 0);
	CHECK(PKTALLOCED(osh) == 0, "pktalloced after pktq %u", PKTALLOCED(osh));

	/* 6. MALLOC and bcmstrbuf */
	{
		struct bcmstrbuf b; char buf[64];
		char *m = MALLOC(osh, 300000);
		CHECK(m != NULL, "MALLOC 300k");
		if (m) { memset(m, 1, 300000); MFREE(osh, m, 300000); }
		CHECK(MALLOCED(osh) == 0, "malloced %u", MALLOCED(osh));
		bcm_binit(&b, buf, sizeof(buf));
		bcm_bprintf(&b, "x=%d", 42);
		CHECK(!strcmp(buf, "x=42"), "bcm_bprintf '%s'", buf);
	}

	/* 7. timers: one-shot and periodic, running under WL_LOCK from softirq */
	wl = wl_selftest_alloc_wl(osh);
	timer_hits = 0;
	t1 = wl_init_timer(wl, st_timer_fn, (void *)1, "oneshot");
	t2 = wl_init_timer(wl, st_timer_fn, (void *)100, "periodic");
	wl_add_timer(wl, t1, 20, FALSE);
	wl_add_timer(wl, t2, 10, TRUE);
	msleep(120);
	CHECK(timer_hits % 100 == 1, "one-shot fired %d", timer_hits % 100);
	CHECK(timer_hits / 100 >= 5 && timer_hits / 100 <= 13, "periodic fired %d", timer_hits / 100);
	wl_del_timer(wl, t2);
	i = timer_hits;
	msleep(50);
	CHECK(timer_hits == i, "periodic still firing after del");
	wl_free_timer(wl, t1);
	wl_free_timer(wl, t2);
	wl_selftest_free_wl(wl);

	osl_detach(osh);
	printk(KERN_INFO "wl selftest: %s (%d failures)\n", st_fail ? "FAILED" : "PASSED", st_fail);
	return st_fail;
}

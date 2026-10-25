/*
 * Linux glue for the prebuilt Broadcom wl core (wlc_blob.o: wl_ap.ko from D-Link DIR-868L B1,
 * SDK 6.37.14.56) on a modern kernel.  Replaces the original wl_linux.c that was compiled
 * into the module.  Modelled after broadcom-sta's wl_linux.c and the decompiled original.
 *
 * Everything the core sees from here (wl_info, wl_if, wl_timer) is opaque to it, so these
 * structs are ours to define.  Packets crossing into the core are shadow wl_skb's
 * (see include/linux_osl.h).
 */
#include <typedefs.h>
#include <linuxver.h>
#include <bcmdefs.h>
#include <osl.h>
#include <bcmutils.h>
#include <bcmendian.h>
#include <wlioctl.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/if_arp.h>
#include <linux/rtnetlink.h>
#include <generated/utsrelease.h>
#include <pcicfg.h>

#define WL_IFTYPE_BSS	1
#define WL_IFTYPE_WDS	2
#define WL_IFTYPE_MON	3
#define ALLPRIO		(-1)
#define WLC_SET_PROMISC	10
#define WLC_SET_MONITOR	108
#define WLC_IOCTL_MAXLEN 8192

struct wl_info;
typedef struct wl_if {
	struct wl_if *next;
	struct wl_info *wl;
	struct net_device *dev;
	void *wlcif;			/* core's wlc_if for this interface */
	uint subunit;
	bool dev_registered;
	int if_type;
	char name[IFNAMSIZ];
	struct work_struct reg_work;
	struct work_struct del_work;
} wl_if_t;

typedef struct wl_timer {
	struct timer_list timer;
	struct wl_info *wl;
	void (*fn)(void *);
	void *arg;
	uint ms;
	bool periodic;
	bool set;
	struct wl_timer *next;
	char name[32];
} wl_timer_t;

typedef struct wl_info {
	uint unit;
	void *pub;			/* wlc_pub_t, opaque, accessed via WLPUB_* below */
	void *wlc;
	osl_t *osh;
	struct net_device *dev;		/* primary netdev */
	wl_if_t *if_list;
	spinlock_t lock;		/* WL_LOCK: serialises every call into the core */
	spinlock_t isr_lock;
	int irq;
	void *regsva;
	bool resched;
	uint32 processed;
	struct tasklet_struct tasklet;
	struct tasklet_struct tx_tasklet;
	atomic_t callbacks;
	wl_timer_t *timers;
	struct pci_dev *pdev;
	struct work_struct mcast_work;
	uint16 vendor, device;
} wl_info_t;

/* fields of the core's wlc_pub_t we need, at the offsets the decompiled original glue used */
#define WLPUB_ETHERADDR(pub)	((u8 *)(pub) + 4)
#define WLPUB_UNIT(pub)		(*(uint *)((char *)(pub) + 0xc))
#define WLPUB_UP(pub)		(*(u8 *)((char *)(pub) + 0x24))
#define WLPUB_WME(pub)		(*(int *)((char *)(pub) + 0x40))
#define WLBSSCFG_ETHERADDR(cfg)	((u8 *)(cfg) + 0xee)

/* the core (wlc_blob.o) */
extern void *wlc_attach(void *wl, uint16 vendor, uint16 device, uint unit, bool piomode, osl_t *osh,
			void *regsva, uint bustype, void *btparam, uint *perr);
extern uint wlc_detach(void *wlc);
extern void *wlc_pub(void *wlc);
extern void *wlc_wlcif_get_by_index(void *wlc, uint idx);
extern void *wlc_bsscfg_find_by_wlcif(void *wlc, void *wlcif);
extern int wlc_iovar_setint(void *wlc, const char *name, int arg);
extern int wlc_iovar_op(void *wlc, const char *name, void *params, int p_len, void *arg, int len,
			bool set, void *wlcif);
extern bool wlc_isr(void *wlc, bool *wantdpc);
extern bool wlc_dpc(void *wlc, bool bounded, uint *processed);
extern void wlc_intrsupd(void *wlc);
extern void wlc_intrson(void *wlc);
extern uint32 wlc_intrsoff(void *wlc);
extern void wlc_intrsrestore(void *wlc, uint32 macintmask);
extern bool wlc_sendpkt(void *wlc, void *p, void *wlcif);
extern int wlc_ioctl(void *wlc, int cmd, void *arg, int len, void *wlcif);
extern int wlc_up(void *wlc);
extern uint wlc_down(void *wlc);
extern void wlc_init(void *wlc);
extern void wlc_reset(void *wlc);
extern bool wlc_chipmatch(uint16 vendor, uint16 device);
extern int wlc_set(void *wlc, int cmd, int arg);
extern void wlblob_compat_init(void);
extern void wlblob_nvram_exit(void);
extern int wlblob_nvram_dev_init(void);

#define WL_LOCK(wl)	spin_lock_bh(&(wl)->lock)
#define WL_UNLOCK(wl)	spin_unlock_bh(&(wl)->lock)
#define WL_ISRLOCK(wl, flags)	spin_lock_irqsave(&(wl)->isr_lock, (flags))
#define WL_ISRUNLOCK(wl, flags)	spin_unlock_irqrestore(&(wl)->isr_lock, (flags))

#define WL_ERROR(args)	printk args
#define WL_INFO(args)	printk args

static int wl_found;
static int instance_base;
static int nompc = 1;
static int skip_chipmatch;
static int ioctl_debug;
module_param(ioctl_debug, int, 0644);
MODULE_PARM_DESC(ioctl_debug, "log every wl ioctl (cmd, len, caller)");
static int selftest;
module_param(selftest, int, 0);
MODULE_PARM_DESC(selftest, "run the glue/OSL self test at probe time (needs no radio)");
module_param(instance_base, int, 0);
module_param(nompc, int, 0);
module_param(skip_chipmatch, int, 0);
MODULE_PARM_DESC(skip_chipmatch, "testing: probe any PCI device bound via new_id, even if the core rejects its ID");
MODULE_PARM_DESC(instance_base, "unit number of the first interface");
MODULE_PARM_DESC(nompc, "disable minimum power consumption (mpc) at attach");

static void wl_free(wl_info_t *wl);
void wl_down(wl_info_t *wl);
void wl_txflowcontrol(wl_info_t *wl, wl_if_t *wlif, bool state, int prio);
uint wl_reset(wl_info_t *wl);
uint32 wl_intrsoff(wl_info_t *wl);
void wl_intrsrestore(wl_info_t *wl, uint32 macintmask);
static void wl_dpc(struct tasklet_struct *t);
static int wl_start(struct sk_buff *skb, struct net_device *dev);

/* ---- netdev private area: just a pointer back to our wl_if ---- */
struct wl_priv_link {
	wl_if_t *wlif;
};
#define WL_DEV_IF(dev)	(((struct wl_priv_link *)netdev_priv(dev))->wlif)
#define WL_INFO_GET(dev)	(WL_DEV_IF(dev)->wl)

/* ------------------------------------------------------------------ interfaces */
static void wl_if_register_work(struct work_struct *work);
static void wl_if_del_work(struct work_struct *work);

static wl_if_t *wl_alloc_if(wl_info_t *wl, int iftype, uint subunit, void *wlcif)
{
	wl_if_t *wlif, *p;
	struct net_device *dev;

	wlif = kzalloc(sizeof(*wlif), GFP_ATOMIC);
	if (!wlif)
		return NULL;
	dev = alloc_etherdev(sizeof(struct wl_priv_link));
	if (!dev) {
		kfree(wlif);
		return NULL;
	}
	wlif->wl = wl;
	wlif->dev = dev;
	wlif->wlcif = wlcif;
	wlif->subunit = subunit;
	wlif->if_type = iftype;
	INIT_WORK(&wlif->reg_work, wl_if_register_work);
	INIT_WORK(&wlif->del_work, wl_if_del_work);
	WL_DEV_IF(dev) = wlif;
	if (iftype == WL_IFTYPE_BSS && wlcif == NULL)
		snprintf(wlif->name, sizeof(wlif->name), "wl%d", wl->unit);
	else
		snprintf(wlif->name, sizeof(wlif->name), "wl%d.%d", wl->unit, subunit);
	/* append to the list */
	if (!wl->if_list)
		wl->if_list = wlif;
	else {
		for (p = wl->if_list; p->next; p = p->next)
			;
		p->next = wlif;
	}
	return wlif;
}

static void wl_free_if(wl_info_t *wl, wl_if_t *wlif)
{
	wl_if_t **pp;

	for (pp = &wl->if_list; *pp; pp = &(*pp)->next) {
		if (*pp == wlif) {
			*pp = wlif->next;
			break;
		}
	}
	if (wlif->dev_registered) {
		unregister_netdev(wlif->dev);
		wlif->dev_registered = FALSE;
	}
	if (wlif->dev)
		free_netdev(wlif->dev);
	kfree(wlif);
}

/* ------------------------------------------------------------------ netdev ops */
int wl_up(wl_info_t *wl)
{
	int error;
	wl_if_t *wlif;

	if (WLPUB_UP(wl->pub))
		return 0;
	error = wlc_up(wl->wlc);
	if (error)
		return error;
	for (wlif = wl->if_list; wlif; wlif = wlif->next)
		wl_txflowcontrol(wl, wlif, OFF, ALLPRIO);
	return 0;
}

void wl_down(wl_info_t *wl)
{
	wl_if_t *wlif;
	uint callbacks, ret_val = 0;
	int i;

	for (wlif = wl->if_list; wlif; wlif = wlif->next)
		if (wlif->dev)
			netif_stop_queue(wlif->dev);
	if (wl->wlc)
		ret_val = wlc_down(wl->wlc);
	callbacks = atomic_read(&wl->callbacks) - ret_val;

	/* wait for timers and tasklets to drain, lock released so they can run */
	WL_UNLOCK(wl);
	for (i = 0; (atomic_read(&wl->callbacks) > callbacks) && i < 10000; i++)
		osl_delay(10);
	WL_LOCK(wl);
}

static int wl_open(struct net_device *dev)
{
	wl_info_t *wl = WL_INFO_GET(dev);
	int error;

	WL_LOCK(wl);
	error = wl_up(wl);
	if (!error)
		error = wlc_set(wl->wlc, WLC_SET_PROMISC, (dev->flags & IFF_PROMISC) ? 1 : 0);
	WL_UNLOCK(wl);
	if (!error)
		netif_start_queue(dev);
	return error ? -ENODEV : 0;
}

static int wl_close(struct net_device *dev)
{
	wl_info_t *wl = WL_INFO_GET(dev);

	netif_stop_queue(dev);
	WL_LOCK(wl);
	if (dev == wl->dev)
		wl_down(wl);
	WL_UNLOCK(wl);
	return 0;
}

static netdev_tx_t wl_start(struct sk_buff *skb, struct net_device *dev)
{
	wl_if_t *wlif = WL_DEV_IF(dev);
	wl_info_t *wl = wlif->wl;
	void *pkt;

	if (skb_headroom(skb) < dev->needed_headroom || skb_cloned(skb) || skb_is_nonlinear(skb)) {
		struct sk_buff *nskb = skb_copy_expand(skb, dev->needed_headroom, 0, GFP_ATOMIC);
		dev_kfree_skb_any(skb);
		if (!nskb)
			return NETDEV_TX_OK;
		skb = nskb;
	}
	skb->next = NULL;
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	WL_LOCK(wl);
	pkt = PKTFRMNATIVE(wl->osh, skb);
	if (!pkt) {
		WL_UNLOCK(wl);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}
	if (WLPUB_WME(wl->pub) && PKTPRIO(pkt) == 0)
		pktsetprio(pkt, FALSE);
	wlc_sendpkt(wl->wlc, pkt, wlif->wlcif);	/* always consumes pkt */
	WL_UNLOCK(wl);
	return NETDEV_TX_OK;
}

static int wl_set_mac_address(struct net_device *dev, void *addr)
{
	wl_if_t *wlif = WL_DEV_IF(dev);
	wl_info_t *wl = wlif->wl;
	struct sockaddr *sa = addr;
	int err;

	WL_LOCK(wl);
	err = wlc_iovar_op(wl->wlc, "cur_etheraddr", NULL, 0, sa->sa_data, ETH_ALEN, TRUE, wlif->wlcif);
	WL_UNLOCK(wl);
	if (!err)
		eth_hw_addr_set(dev, sa->sa_data);
	return err ? -EINVAL : 0;
}

static void wl_mcast_work(struct work_struct *work)
{
	wl_info_t *wl = container_of(work, wl_info_t, mcast_work);
	struct net_device *dev = wl->dev;

	WL_LOCK(wl);
	if (wl->wlc) {
		wlc_set(wl->wlc, WLC_SET_PROMISC, (dev->flags & IFF_PROMISC) ? 1 : 0);
		wlc_iovar_setint(wl->wlc, "allmulti", (dev->flags & IFF_ALLMULTI) ? 1 : 0);
	}
	WL_UNLOCK(wl);
	atomic_dec(&wl->callbacks);
}

static void wl_set_rx_mode(struct net_device *dev)
{
	wl_info_t *wl = WL_INFO_GET(dev);

	if (dev == wl->dev) {
		atomic_inc(&wl->callbacks);
		if (!schedule_work(&wl->mcast_work))
			atomic_dec(&wl->callbacks);
	}
}

/* Broadcom private ioctl: SIOCDEVPRIVATE (0x89f0) carries a wl_ioctl_t */
static int wl_siocdevprivate(struct net_device *dev, struct ifreq *ifr, void __user *data, int cmd)
{
	wl_if_t *wlif = WL_DEV_IF(dev);
	wl_info_t *wl = wlif->wl;
	wl_ioctl_t ioc;
	void *buf = NULL;
	uint buflen;
	int bcmerror = 0;

	if (cmd != SIOCDEVPRIVATE)
		return -EOPNOTSUPP;
	if (copy_from_user(&ioc, data, sizeof(ioc)))
		return -EFAULT;
	if (ioctl_debug)
		printk(KERN_INFO "wl: ioctl %s cmd=%d len=%d set=%d (%s)\n", dev->name, ioc.cmd, ioc.len, ioc.set, current->comm);
	if (ioc.len > 0x100000)
		return -EINVAL;
	buflen = max_t(uint, ioc.len, WLC_IOCTL_MAXLEN);
	if (ioc.buf) {
		buf = MALLOC(wl->osh, buflen);
		if (!buf)
			return -ENOMEM;
		if (copy_from_user(buf, (void __user *)ioc.buf, ioc.len)) {
			bcmerror = -EFAULT;
			goto done;
		}
	}
	if (ioc.set && !capable(CAP_NET_ADMIN)) {
		bcmerror = -EPERM;
		goto done;
	}
	WL_LOCK(wl);
	bcmerror = wlc_ioctl(wl->wlc, ioc.cmd, buf, ioc.len, wlif->wlcif);
	WL_UNLOCK(wl);
	if (ioc.buf && copy_to_user((void __user *)ioc.buf, buf, ioc.len))
		bcmerror = -EFAULT;
	bcmerror = OSL_ERROR(bcmerror);
done:
	if (buf)
		MFREE(wl->osh, buf, buflen);
	return bcmerror;
}

static const struct net_device_ops wl_netdev_ops = {
	.ndo_open = wl_open,
	.ndo_stop = wl_close,
	.ndo_start_xmit = wl_start,
	.ndo_set_mac_address = wl_set_mac_address,
	.ndo_set_rx_mode = wl_set_rx_mode,
	.ndo_siocdevprivate = wl_siocdevprivate,
};

static int wl_register_if(wl_info_t *wl, wl_if_t *wlif, const u8 *addr)
{
	struct net_device *dev = wlif->dev;
	int err;

	strscpy(dev->name, wlif->name, IFNAMSIZ);
	dev->netdev_ops = &wl_netdev_ops;
	dev->needed_headroom = 256;
	dev->needed_tailroom = 32;
	eth_hw_addr_set(dev, addr);
	SET_NETDEV_DEV(dev, &wl->pdev->dev);
	err = register_netdev(dev);
	if (err) {
		WL_ERROR((KERN_ERR "wl%d: register_netdev(%s) failed %d\n", wl->unit, wlif->name, err));
		return err;
	}
	wlif->dev_registered = TRUE;
	return 0;
}

/* virtual interfaces are created/deleted by the core under WL_LOCK: finish in process context */
static void wl_if_register_work(struct work_struct *work)
{
	wl_if_t *wlif = container_of(work, wl_if_t, reg_work);
	wl_info_t *wl = wlif->wl;
	void *cfg;
	u8 addr[ETH_ALEN];

	WL_LOCK(wl);
	cfg = wlc_bsscfg_find_by_wlcif(wl->wlc, wlif->wlcif);
	if (cfg)
		memcpy(addr, WLBSSCFG_ETHERADDR(cfg), ETH_ALEN);
	else
		memcpy(addr, WLPUB_ETHERADDR(wl->pub), ETH_ALEN);
	WL_UNLOCK(wl);
	/* register_netdev() takes RTNL itself; taking it here deadlocked the worker */
	wl_register_if(wl, wlif, addr);
	atomic_dec(&wl->callbacks);
}

static void wl_if_del_work(struct work_struct *work)
{
	wl_if_t *wlif = container_of(work, wl_if_t, del_work);
	wl_info_t *wl = wlif->wl;

	wl_if_t **pp;

	/* unlink under the core lock, then tear the netdev down without any lock held:
	 * unregister_netdev() sleeps and takes RTNL on its own */
	WL_LOCK(wl);
	for (pp = &wl->if_list; *pp; pp = &(*pp)->next)
		if (*pp == wlif) {
			*pp = wlif->next;
			break;
		}
	WL_UNLOCK(wl);
	wlif->next = NULL;
	wl_free_if(wl, wlif);
	atomic_dec(&wl->callbacks);
}

/* ------------------------------------------------------------------ wl_export.h: core -> glue */
struct wl_if *wl_add_if(wl_info_t *wl, void *wlcif, uint unit, const u8 *remote)
{
	wl_if_t *wlif = wl_alloc_if(wl, remote ? WL_IFTYPE_WDS : WL_IFTYPE_BSS, unit, wlcif);

	if (!wlif)
		return NULL;
	if (remote)
		snprintf(wlif->name, sizeof(wlif->name), "wds%d.%d", wl->unit, unit);
	atomic_inc(&wl->callbacks);
	schedule_work(&wlif->reg_work);
	return wlif;
}

void wl_del_if(wl_info_t *wl, wl_if_t *wlif)
{
	wlif->wlcif = NULL;
	atomic_inc(&wl->callbacks);
	schedule_work(&wlif->del_work);
}

void wl_dwds_del_if(wl_info_t *wl, wl_if_t *wlif, bool force)
{
}

char *wl_ifname(wl_info_t *wl, wl_if_t *wlif)
{
	return wlif ? wlif->name : wl->dev->name;
}

void *wl_get_ifctx(wl_info_t *wl, int ctx_id, wl_if_t *wlif)
{
	if (ctx_id == 3)	/* IFCTX_NETDEV */
		return wlif ? wlif->dev : wl->dev;
	return NULL;
}

void wl_init(wl_info_t *wl)
{
	wl_reset(wl);
	wlc_init(wl->wlc);
}

uint wl_reset(wl_info_t *wl)
{
	uint32 macintmask = wl_intrsoff(wl);

	wlc_reset(wl->wlc);
	wl_intrsrestore(wl, macintmask);
	wl->resched = FALSE;
	return 0;
}

void wl_intrson(wl_info_t *wl)
{
	unsigned long flags;

	WL_ISRLOCK(wl, flags);
	wlc_intrson(wl->wlc);
	WL_ISRUNLOCK(wl, flags);
}

uint32 wl_intrsoff(wl_info_t *wl)
{
	unsigned long flags;
	uint32 status;

	WL_ISRLOCK(wl, flags);
	status = wlc_intrsoff(wl->wlc);
	WL_ISRUNLOCK(wl, flags);
	return status;
}

void wl_intrsrestore(wl_info_t *wl, uint32 macintmask)
{
	unsigned long flags;

	WL_ISRLOCK(wl, flags);
	wlc_intrsrestore(wl->wlc, macintmask);
	WL_ISRUNLOCK(wl, flags);
}

bool wl_alloc_dma_resources(wl_info_t *wl, uint addrwidth)
{
	return TRUE;
}

void wl_dump_ver(wl_info_t *wl, struct bcmstrbuf *b)
{
	bcm_bprintf(b, "wl%d: glue for kernel " UTS_RELEASE "\n", wl->unit);
}

void wl_txflowcontrol(wl_info_t *wl, wl_if_t *wlif, bool state, int prio)
{
	struct net_device *dev = wlif ? wlif->dev : wl->dev;

	if (!dev || (wlif && !wlif->dev_registered))
		return;
	if (state == ON)
		netif_stop_queue(dev);
	else
		netif_wake_queue(dev);
}

void wl_event(wl_info_t *wl, char *ifname, void *e)
{
}

void wl_event_sync(wl_info_t *wl, char *ifname, void *e)
{
}

void wl_monitor(wl_info_t *wl, void *rxsts, void *p)
{
	PKTFREE(wl->osh, p, FALSE);
}

void wl_set_monitor(wl_info_t *wl, int val)
{
}

/* rx: hand (possibly chained) packets to the stack */
void wl_sendup(wl_info_t *wl, wl_if_t *wlif, void *p, int numpkt)
{
	struct net_device *dev = wlif ? wlif->dev : wl->dev;

	if (!dev || (wlif && (!wlif->dev_registered || !(dev->flags & IFF_UP)))) {
		PKTCFREE(wl->osh, p, FALSE);
		return;
	}
	while (p) {
		void *next = PKTISCHAINED(p) ? PKTCLINK(p) : NULL;
		struct sk_buff *skb;

		PKTCLRCHAINED(wl->osh, p);
		PKTSETCLINK(p, NULL);
		PKTSETNEXT(wl->osh, p, NULL);
		WLSKB(p)->dev = dev;
		skb = PKTTONATIVE(wl->osh, p);
		skb->protocol = eth_type_trans(skb, dev);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;
		netif_rx(skb);
		p = next;
	}
}

/* ------------------------------------------------------------------ timers */
static void wl_timer_fn(struct timer_list *tl)
{
#ifdef timer_container_of	/* 6.16+; from_timer() is gone in 6.17 */
	wl_timer_t *t = timer_container_of(t, tl, timer);
#else
	wl_timer_t *t = from_timer(t, tl, timer);
#endif
	wl_info_t *wl = t->wl;

	WL_LOCK(wl);
	if (t->set) {
		if (t->periodic) {
			mod_timer(&t->timer, jiffies + msecs_to_jiffies(t->ms));
			atomic_inc(&wl->callbacks);
		} else {
			t->set = FALSE;
		}
		t->fn(t->arg);
	}
	atomic_dec(&wl->callbacks);
	WL_UNLOCK(wl);
}

wl_timer_t *wl_init_timer(wl_info_t *wl, void (*fn)(void *arg), void *arg, const char *name)
{
	wl_timer_t *t = kzalloc(sizeof(*t), GFP_ATOMIC);

	if (!t)
		return NULL;
	timer_setup(&t->timer, wl_timer_fn, 0);
	t->wl = wl;
	t->fn = fn;
	t->arg = arg;
	if (name)
		strscpy(t->name, name, sizeof(t->name));
	t->next = wl->timers;
	wl->timers = t;
	return t;
}

void wl_add_timer(wl_info_t *wl, wl_timer_t *t, uint ms, int periodic)
{
	t->ms = ms;
	t->periodic = (bool)periodic;
	if (!t->set) {
		t->set = TRUE;
		atomic_inc(&wl->callbacks);
		mod_timer(&t->timer, jiffies + msecs_to_jiffies(ms));
	}
}

bool wl_del_timer(wl_info_t *wl, wl_timer_t *t)
{
	if (t->set) {
		t->set = FALSE;
		if (timer_delete(&t->timer))
			atomic_dec(&wl->callbacks);
	}
	return TRUE;
}

void wl_free_timer(wl_info_t *wl, wl_timer_t *t)
{
	wl_timer_t **pp;

	wl_del_timer(wl, t);
	for (pp = &wl->timers; *pp; pp = &(*pp)->next) {
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	}
	kfree(t);
}

/* ------------------------------------------------------------------ interrupts */
static irqreturn_t wl_isr(int irq, void *dev_id)
{
	wl_info_t *wl = dev_id;
	unsigned long flags;
	bool ours, wantdpc = FALSE;

	WL_ISRLOCK(wl, flags);
	ours = wlc_isr(wl->wlc, &wantdpc);
	if (ours && wantdpc)
		tasklet_schedule(&wl->tasklet);
	WL_ISRUNLOCK(wl, flags);
	return IRQ_RETVAL(ours);
}

static void wl_dpc(struct tasklet_struct *t)
{
	wl_info_t *wl = from_tasklet(wl, t, tasklet);

	WL_LOCK(wl);
	if (!WLPUB_UP(wl->pub)) {
		WL_UNLOCK(wl);
		return;
	}
	if (wl->resched) {
		unsigned long flags;
		WL_ISRLOCK(wl, flags);
		wlc_intrsupd(wl->wlc);
		WL_ISRUNLOCK(wl, flags);
	}
	wl->resched = wlc_dpc(wl->wlc, TRUE, &wl->processed);
	if (WLPUB_UP(wl->pub)) {
		if (wl->resched)
			tasklet_schedule(&wl->tasklet);
		else
			wl_intrson(wl);
	}
	WL_UNLOCK(wl);
}

/* ------------------------------------------------------------------ attach / detach */
static wl_info_t *wl_attach(uint16 vendor, uint16 device, ulong regs, struct pci_dev *pdev, uint irq)
{
	wl_info_t *wl;
	osl_t *osh;
	wl_if_t *wlif;
	uint err, unit = instance_base + wl_found;

	osh = osl_attach(pdev, PCI_BUS, TRUE);
	if (!osh)
		return NULL;
	wl = kzalloc(sizeof(*wl), GFP_KERNEL);
	if (!wl) {
		osl_detach(osh);
		return NULL;
	}
	wl->unit = unit;
	wl->osh = osh;
	wl->pdev = pdev;
	wl->vendor = vendor;
	wl->device = device;
	spin_lock_init(&wl->lock);
	spin_lock_init(&wl->isr_lock);
	tasklet_setup(&wl->tasklet, wl_dpc);
	INIT_WORK(&wl->mcast_work, wl_mcast_work);

	wlif = wl_alloc_if(wl, WL_IFTYPE_BSS, unit, NULL);
	if (!wlif)
		goto fail;
	wl->dev = wlif->dev;

	wl->regsva = ioremap(regs, PCI_BAR0_WINSZ);
	if (!wl->regsva) {
		WL_ERROR((KERN_ERR "wl%d: ioremap failed\n", unit));
		goto fail;
	}

	wl->wlc = wlc_attach(wl, vendor, device, unit, FALSE, osh, wl->regsva, PCI_BUS, pdev, &err);
	if (!wl->wlc) {
		WL_ERROR((KERN_ERR "wl%d: wlc_attach failed with code %d\n", unit, err));
		goto fail;
	}
	wl->pub = wlc_pub(wl->wlc);
	wlif->wlcif = wlc_wlcif_get_by_index(wl->wlc, 0);
	if (nompc)
		wlc_iovar_setint(wl->wlc, "mpc", 0);

	if (request_irq(irq, wl_isr, IRQF_SHARED, wlif->name, wl)) {
		WL_ERROR((KERN_ERR "wl%d: request_irq(%d) failed\n", unit, irq));
		goto fail;
	}
	wl->irq = irq;
	if (wl_register_if(wl, wlif, WLPUB_ETHERADDR(wl->pub)))
		goto fail;

	WL_INFO((KERN_INFO "%s: Broadcom BCM%04x 802.11 Wireless Controller (re-linked wl core)\n",
		 wlif->name, device));
	wl_found++;
	return wl;
fail:
	wl_free(wl);
	return NULL;
}

static void wl_free(wl_info_t *wl)
{
	wl_timer_t *t, *next;

	if (wl->irq)
		free_irq(wl->irq, wl);
	tasklet_kill(&wl->tasklet);
	if (wl->wlc) {
		/* no lock here: the irq and tasklet are gone, and wlc_detach() frees coherent
		 * DMA rings, which vunmap()s and must not run with bottom halves disabled */
		wlc_detach(wl->wlc);
		wl->wlc = NULL;
	}
	while (atomic_read(&wl->callbacks) > 0)	/* pending works and timers */
		schedule_timeout_uninterruptible(1);
	while (wl->if_list)
		wl_free_if(wl, wl->if_list);
	for (t = wl->timers; t; t = next) {
		next = t->next;
		timer_delete_sync(&t->timer);
		kfree(t);
	}
	if (wl->regsva)
		iounmap(wl->regsva);
	if (osl_malloced(wl->osh))
		WL_ERROR((KERN_WARNING "wl%d: Memory leak of bytes %d\n", wl->unit, osl_malloced(wl->osh)));
	osl_detach(wl->osh);
	kfree(wl);
}

/* ------------------------------------------------------------------ self test hooks */
extern int wl_selftest(struct pci_dev *pdev);
struct wl_info *wl_selftest_alloc_wl(osl_t *osh)
{
	wl_info_t *wl = kzalloc(sizeof(*wl), GFP_KERNEL);
	spin_lock_init(&wl->lock);
	spin_lock_init(&wl->isr_lock);
	wl->osh = osh;
	return wl;
}
void wl_selftest_free_wl(struct wl_info *wl)
{
	kfree(wl);
}

/* ------------------------------------------------------------------ PCI */
extern int wlblob_nvram_load_file(struct device *dev);

static int wl_pci_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	wl_info_t *wl;
	int rc;

	if (pdev->vendor != PCI_VENDOR_ID_BROADCOM && !skip_chipmatch)
		return -ENODEV;
	if (!wlc_chipmatch(pdev->vendor, pdev->device) && !skip_chipmatch) {
		WL_INFO((KERN_INFO "wl: %04x:%04x not supported by this core\n", pdev->vendor, pdev->device));
		return -ENODEV;
	}
	rc = pci_enable_device(pdev);
	if (rc)
		return rc;
	wlblob_nvram_load_file(&pdev->dev);
	pci_set_master(pdev);
	if (selftest && wl_selftest(pdev))
		WL_ERROR((KERN_ERR "wl: self test failed, continuing anyway\n"));
	if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32))) {
		WL_ERROR((KERN_ERR "wl: no 32 bit DMA\n"));
		pci_disable_device(pdev);
		return -ENODEV;
	}
	wl = wl_attach(pdev->vendor, pdev->device, pci_resource_start(pdev, 0), pdev, pdev->irq);
	if (!wl) {
		pci_disable_device(pdev);
		return -ENODEV;
	}
	pci_set_drvdata(pdev, wl);
	return 0;
}

static void wl_pci_remove(struct pci_dev *pdev)
{
	wl_info_t *wl = pci_get_drvdata(pdev);

	if (!wl)
		return;
	WL_LOCK(wl);
	wl_down(wl);
	WL_UNLOCK(wl);
	wl_free(wl);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id wl_id_table[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0x4360) },	/* BCM4360 without SROM (raw id, DIR-868L B1 5GHz) */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0x43a1) },	/* BCM4360 2.4GHz  (DIR-868L B1 wl0) */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0x43a2) },	/* BCM4360 5GHz    (DIR-868L B1 wl1) */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0x43a0) },	/* BCM4360 dual */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0x43b1) },	/* BCM4352 */
	{ PCI_DEVICE(PCI_VENDOR_ID_BROADCOM, 0x4331) },	/* BCM4331 */
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, wl_id_table);

static struct pci_driver wl_pci_driver = {
	.name = "wl",
	.id_table = wl_id_table,
	.probe = wl_pci_probe,
	.remove = wl_pci_remove,
};

static int __init wl_module_init(void)
{
	wlblob_compat_init();
	wlblob_nvram_dev_init();
#ifdef WLSKB_LAYOUT_FT
	printk(KERN_INFO "wl: Broadcom wl core 6.37.14.126 (FreshTomato wl_apsta.o) re-linked for kernel " UTS_RELEASE "\n");
#else
	printk(KERN_INFO "wl: Broadcom wl core 6.37.14.56 (r428735) re-linked for kernel " UTS_RELEASE "\n");
#endif
	return pci_register_driver(&wl_pci_driver);
}

static void __exit wl_module_exit(void)
{
	pci_unregister_driver(&wl_pci_driver);
	wlblob_nvram_exit();
}

module_init(wl_module_init);
module_exit(wl_module_exit);
MODULE_LICENSE("Proprietary");
MODULE_DESCRIPTION("Broadcom 802.11 wireless (prebuilt wl core + new Linux glue)");

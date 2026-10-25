/* BCM4360 TX checksum offload, FT 6.37.14.126 only.
 *
 * Only IPv4 TCP is advertised. Other protocols use Linux's software fallback.
 * Slow preparation completes partial checksums BEFORE fragmentation/encryption.
 * Chained frames also use software: one TOE checksum header cannot describe
 * multiple A-MSDU subframes. The cached, single-packet fast path uses hardware.
 * All cached TOE headers are refreshed before DMA cache maintenance.
 */
#include <typedefs.h>
#include <linuxver.h>
#include <bcmdefs.h>
#include <osl.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <net/checksum.h>
#include "wl_cso.h"

bool cso_offload = true;
module_param(cso_offload, bool, 0400);
MODULE_PARM_DESC(cso_offload, "BCM4360 IPv4 TCP checksum offload (requires module reload)");
static bool cso_stack = true;
module_param(cso_stack, bool, 0644);
MODULE_PARM_DESC(cso_stack, "allow Linux partial checksums (requires cso_offload)");
static unsigned int cso_hw, cso_sw, cso_invalid, cso_slow, cso_chained;
module_param(cso_hw, uint, 0400);
module_param(cso_sw, uint, 0400);
module_param(cso_invalid, uint, 0400);
module_param(cso_slow, uint, 0400);
module_param(cso_chained, uint, 0400);

/* Bounds refer to the native allocation, since the blob moves shadow data.
 * Network header offsets remain attached to the real skb through skb copies.
 */
static u8 *cso_ip(struct sk_buff *skb)
{
	u8 *ip;
	unsigned int len, tcp_len;
	if (!skb || skb->protocol != htons(ETH_P_IP) || skb_is_nonlinear(skb))
		return NULL;
	ip = skb_network_header(skb);
	if (ip < skb->head || ip > skb_tail_pointer(skb) ||
	    skb_tail_pointer(skb) - ip < 40 || ip[0] != 0x45 ||
	    ip[9] != IPPROTO_TCP || (ip[6] & 0x3f) || ip[7])
		return NULL;
	len = (ip[2] << 8) | ip[3];
	tcp_len = (ip[32] >> 4) * 4;
	if (tcp_len < 20 || len < 20 + tcp_len || len > skb_tail_pointer(skb) - ip)
		return NULL;
	return ip;
}

netdev_features_t wl_cso_features_check(struct sk_buff *skb,
	struct net_device *dev, netdev_features_t features)
{
	unsigned int off = skb_network_offset(skb), len, thlen;
	const u8 *ip;
	if (!cso_offload || !READ_ONCE(cso_stack) || skb->encapsulation ||
	    skb_vlan_tag_present(skb) || skb->protocol != htons(ETH_P_IP) ||
	    off > skb_headlen(skb) || skb_headlen(skb) - off < 40)
		goto software;
	ip = skb_network_header(skb);
	len = (ip[2] << 8) | ip[3];
	thlen = (ip[32] >> 4) * 4;
	if (ip[0] != 0x45 || ip[9] != IPPROTO_TCP || (ip[6] & 0x3f) || ip[7] ||
	    thlen < 20 || len < 20 + thlen || len > skb->len - off ||
	    skb_headlen(skb) - off < 20 + thlen)
		goto software;
	if (skb->ip_summed == CHECKSUM_PARTIAL &&
	    (skb_checksum_start_offset(skb) != off + 20 || skb->csum_offset != 16))
		goto software;
	return features;
software:
	return features & ~NETIF_F_CSUM_MASK;
}

static bool cso_complete(struct wl_skb *p)
{
	struct sk_buff *skb = p->native;
	u8 *ip, *tcp;
	unsigned int len;
	__be32 src, dst;
	__sum16 sum;
	if (!skb || skb->ip_summed != CHECKSUM_PARTIAL)
		return true;
	ip = cso_ip(skb);
	if (!ip) {
		cso_invalid++;
		return false;
	}
	tcp = ip + 20;
	len = ((ip[2] << 8) | ip[3]) - 20;
	memcpy(&src, ip + 12, 4);
	memcpy(&dst, ip + 16, 4);
	tcp[16] = tcp[17] = 0;
	sum = csum_tcpudp_magic(src, dst, len, IPPROTO_TCP, csum_partial(tcp, len, 0));
	memcpy(tcp + 16, &sum, 2);
	ip[10] = ip[11] = 0;
	sum = ip_compute_csum(ip, 20);
	memcpy(ip + 10, &sum, 2);
	skb->ip_summed = CHECKSUM_NONE;
	p->flags1 &= ~WLSKB_IPSUMMED_MASK;
	cso_sw++;
	return true;
}

static bool cso_complete_chain(struct wl_skb *p)
{
	for (; p; p = p->next)
		if (!cso_complete(p))
			return false;
	return true;
}

extern int wlblob_original_wlc_prep_sdu(void *, void **, int *, unsigned int *);
extern int wlblob_original_wlc_prep_sdu_fast(void *, void *, void *, void *, unsigned int *);

int wlc_prep_sdu(void *wlc, void **pkts, int *npkts, unsigned int *fifo)
{
	if (cso_offload) {
		cso_slow++;
		if (!cso_complete_chain(*pkts)) {
			/* FT wlc_info: pub at 0, osh at 4 (verified in original prologue). */
			osl_pktfree(((void **)wlc)[1], *pkts, true);
			*npkts = 0;
			return -1;
		}
	}
	return wlblob_original_wlc_prep_sdu(wlc, pkts, npkts, fifo);
}

int wlc_prep_sdu_fast(void *wlc, void *cfg, void *scb, void *pkt, unsigned int *fifo)
{
	struct wl_skb *p = pkt;
	if (cso_offload && p->next) {
		cso_chained++;
		if (!cso_complete_chain(p))
			return -23; /* BCME_UNSUPPORTED: caller retries slow preparation. */
	}
	return wlblob_original_wlc_prep_sdu_fast(wlc, cfg, scb, pkt, fifo);
}

void wlc_toe_add_hdr(void *wlc, void *pkt, void *scb, void *key,
	unsigned int nfrags, uint16 *pushlen)
{
	struct wl_skb *p = pkt;
	u8 hdr[16] = { 2 };
	unsigned int len = 4;
	if (cso_offload) {
		len = sizeof(hdr);
		hdr[0] = 3; /* full-length passthrough, safe to cache */
		hdr[2] = p->data[2] & 1; /* short D11 TX descriptor */
	}
	memcpy(osl_pktpush(NULL, p, len), hdr, len);
	if (pushlen)
		*pushlen = len;
}

void wl_cso_dma(void *va, unsigned int size, void *pkt)
{
	struct wl_skb *p = pkt;
	struct sk_buff *skb;
	u8 *d = va, *ip;
	unsigned int iplen, thlen, offset, payload, d11len, flags;
	if (!cso_offload || !p || p->data != va || size < 40)
		return;
	memcpy(&flags, p->cb, sizeof(flags));
	if (!(flags & 0x80)) /* WLF_TXHDR; exclude later A-MSDU buffers */
		goto software;
	if (!((d[0] == 3 && d[1] == 0) || (d[0] == 0x51 && d[1] == 7)))
		goto software;
	d11len = (d[18] & 1) ? 20 : 124;
	/* The cached header may belong to a previously offloaded packet. */
	memset(d, 0, 16);
	d[0] = 3;
	d[2] = d11len == 20;
	skb = p->native;
	if (p->next || p->data_len || (flags & 0x40) || !skb ||
	    skb->ip_summed != CHECKSUM_PARTIAL || !(ip = cso_ip(skb)))
		goto software;
	if (size < 16 + d11len + 24 + 8 + 40 ||
	    ip < d + 16 + d11len + 24 + 8 || ip > d + size - 40 ||
	    (d[16 + d11len] & 0x0c) != 0x08 ||
	    memcmp(ip - 8, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8))
		goto software;
	iplen = (ip[2] << 8) | ip[3];
	thlen = (ip[32] >> 4) * 4;
	offset = ip - d - 16;
	if (iplen > d + size - ip || offset > 224)
		goto software;
	payload = iplen - 20 - thlen;
	d[0] = 0x51; /* normal header, IPv4, TCP */
	d[1] = 7; /* IP, TCP, pseudo-header checksums */
	d[4] = d[8] = payload;
	d[5] = d[9] = payload >> 8;
	d[12] = offset;
	d[14] = offset + 20;
	ip[10] = ip[11] = ip[36] = ip[37] = 0;
	cso_hw++;
	return;
software:
	cso_complete(p);
}

/* Exercise header-cache transitions and software fallback without DMA. */
int wl_cso_selftest(void)
{
	struct sk_buff *skb;
	struct wl_skb p = { 0 }, second = { 0 };
	u8 *d, *ip, *tcp;
	__be32 src, dst;
	__sum16 saved;
	unsigned int flags = 0x80;
	int fail = 0, i;
	if (!cso_offload)
		return 0;
	skb = alloc_skb(512, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;
	d = skb_put_zero(skb, 239);
	skb->protocol = htons(ETH_P_IP);
	skb_set_network_header(skb, 182);
	ip = d + 182;
	tcp = ip + 20;
	ip[0] = 0x45; ip[3] = 57; ip[8] = 64; ip[9] = 6;
	ip[12] = 192; ip[13] = 0; ip[14] = 2; ip[15] = 3;
	ip[16] = 198; ip[17] = 51; ip[18] = 100; ip[19] = 7;
	tcp[0] = 1; tcp[1] = 187; tcp[2] = 123; tcp[3] = 45; tcp[12] = 0x50;
	for (i = 0; i < 17; i++)
		tcp[20 + i] = i * 7;
	memcpy(&src, ip + 12, 4); memcpy(&dst, ip + 16, 4);
	/* Linux must finish every unsupported checksum before ndo_start_xmit. */
	skb->ip_summed = CHECKSUM_PARTIAL;
	skb->csum_start = skb_headroom(skb) + 202;
	skb->csum_offset = 16;
	if (READ_ONCE(cso_stack)) {
		fail += !(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		ip[9] = IPPROTO_UDP;
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		ip[9] = IPPROTO_TCP; ip[6] = 0x20;
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		ip[6] = 0; ip[0] = 0x46;
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		ip[0] = 0x45; skb->csum_offset = 8;
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		skb->csum_offset = 16; skb->encapsulation = 1;
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		skb->encapsulation = 0;
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), 1);
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		__vlan_hwaccel_clear_tag(skb);
		skb->protocol = htons(ETH_P_IPV6);
		fail += !!(wl_cso_features_check(skb, NULL, NETIF_F_HW_CSUM) & NETIF_F_HW_CSUM);
		skb->protocol = htons(ETH_P_IP);
	}
	p.native = skb; p.head = skb->head; p.data = d; p.len = skb->len;
	memcpy(p.cb, &flags, 4);
	d[0] = 3; d[140] = 0x88;
	memcpy(ip - 8, "\xaa\xaa\x03\x00\x00\x00\x08\x00", 8);
	skb->ip_summed = CHECKSUM_PARTIAL;
	wl_cso_dma(d, p.len, &p);
	fail += d[0] != 0x51 || d[1] != 7 || d[4] != 17 || d[8] != 17 ||
		d[12] != 166 || d[14] != 186 || ip[10] || ip[11] || tcp[16] || tcp[17];
	fail += !cso_complete(&p);
	fail += ip_compute_csum(ip, 20) != 0 ||
		csum_tcpudp_magic(src, dst, 37, 6, csum_partial(tcp, 37, 0)) != 0;
	memcpy(&saved, tcp + 16, 2);
	/* A software-completed packet must clear the cached HW request. */
	wl_cso_dma(d, p.len, &p);
	fail += d[0] != 3 || d[1] != 0 || memcmp(tcp + 16, &saved, 2);
	/* Chained packets use software, including when a cached HW header exists. */
	skb->ip_summed = CHECKSUM_PARTIAL;
	d[0] = 0x51; d[1] = 7; p.next = &second;
	tcp[16] = tcp[17] = 0;
	wl_cso_dma(d, p.len, &p);
	fail += d[0] != 3 || d[1] != 0 || skb->ip_summed != CHECKSUM_NONE ||
		csum_tcpudp_magic(src, dst, 37, 6, csum_partial(tcp, 37, 0)) != 0;
	/* Reject truncated IP/TCP headers before constructing a hardware request. */
	ip[2] = 4;
	fail += cso_ip(skb) != NULL;
	ip[2] = 0; tcp[12] = 0xf0;
	fail += cso_ip(skb) != NULL;
	kfree_skb(skb);
	printk(KERN_INFO "wl cso selftest: %s (%d failures)\n", fail ? "FAILED" : "PASSED", fail);
	return fail ? -EINVAL : 0;
}

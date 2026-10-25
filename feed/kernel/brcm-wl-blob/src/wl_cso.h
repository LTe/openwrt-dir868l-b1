#ifndef WL_CSO_H
#define WL_CSO_H
#include <linux/netdevice.h>
#ifdef WLSKB_LAYOUT_FT
extern bool cso_offload;
netdev_features_t wl_cso_features_check(struct sk_buff *, struct net_device *, netdev_features_t);
void wl_cso_dma(void *, unsigned int, void *);
int wl_cso_selftest(void);
#else
/* The D-Link object has different code offsets and is never interposed. */
#define cso_offload false
static inline netdev_features_t wl_cso_features_check(struct sk_buff *skb,
	struct net_device *dev, netdev_features_t features)
{
	return features & ~NETIF_F_CSUM_MASK;
}
static inline void wl_cso_dma(void *va, unsigned int size, void *pkt) {}
static inline int wl_cso_selftest(void) { return 0; }
#endif
#endif

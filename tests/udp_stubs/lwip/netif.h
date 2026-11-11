#ifndef TEST_NETIF_H
#define TEST_NETIF_H
#include "udp.h"
struct netif { uint16_t mtu; uint8_t up, link; ip4_addr_t ip, mask, gateway; };
#define netif_is_up(n) ((n)->up)
#define netif_is_link_up(n) ((n)->link)
void netif_set_addr(struct netif *, const ip4_addr_t *, const ip4_addr_t *, const ip4_addr_t *);
#endif

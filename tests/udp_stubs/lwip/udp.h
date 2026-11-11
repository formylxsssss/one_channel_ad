#ifndef TEST_UDP_H
#define TEST_UDP_H
#include <stdint.h>
typedef int err_t;
typedef uint16_t u16_t;
typedef struct { uint32_t addr; } ip_addr_t;
typedef ip_addr_t ip4_addr_t;
#define IP4_ADDR(p,a,b,c,d) ((p)->addr = (uint32_t)(a) | ((uint32_t)(b)<<8) | ((uint32_t)(c)<<16) | ((uint32_t)(d)<<24))
#define ip_addr_cmp(a,b) ((a)->addr == (b)->addr)
#define ERR_OK 0
#define ERR_MEM -1
#define ERR_ARG -16
#define ERR_CONN -11
struct pbuf;
struct udp_pcb;
typedef void (*udp_recv_fn)(void *, struct udp_pcb *, struct pbuf *, const ip_addr_t *, u16_t);
struct udp_pcb { ip_addr_t remote_ip; uint16_t remote_port, local_port; udp_recv_fn recv; };
struct udp_pcb *udp_new(void);
err_t udp_connect(struct udp_pcb *, const ip_addr_t *, uint16_t);
void udp_recv(struct udp_pcb *, udp_recv_fn, void *);
void udp_remove(struct udp_pcb *);
err_t udp_send(struct udp_pcb *, struct pbuf *);
#endif

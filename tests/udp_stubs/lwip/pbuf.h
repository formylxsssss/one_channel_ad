#ifndef TEST_PBUF_H
#define TEST_PBUF_H
#include "udp.h"
#define PBUF_TRANSPORT 0
#define PBUF_RAM 0
struct pbuf { void *payload; uint16_t len, tot_len; };
struct pbuf *pbuf_alloc(int, uint16_t, int);
err_t pbuf_take(struct pbuf *, const void *, uint16_t);
uint16_t pbuf_copy_partial(const struct pbuf *, void *, uint16_t, uint16_t);
uint8_t pbuf_free(struct pbuf *);
#endif

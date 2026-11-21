/* gw_link.h -- Gunmen Wars' cabinet link (src/gw_link.c; design: LINK.md) as the GMEN board's chip model (src/gw_gmen.c) sees it */
#ifndef GW_LINK_H
#define GW_LINK_H
#include <stdint.h>

enum { GW_PKT_ASYNC = 0, GW_PKT_ISO = 1 };      /* what a bus packet is: an asynchronous one (to a node, or all) or an isochronous one */
#define GW_PKT_MAXQ 300                         /* the largest packet carried, in quadlets (header + data + the status word) */

int  gw_link_nodes(void);                       /* the cabinets on the bus while linked; 0 = not linked (one cabinet: the chip pretends) */
int  gw_link_node(void);                        /* this cabinet's node (PHY) number, 0..nodes-1 */
/* a packet onto the bus, as the receiving chip will see it (receive format, status word last), stamped with the sender's game time t */
void gw_link_send(int kind, int dst_node, const uint32_t *q, int n, uint64_t t);
/* the bus's packets due by game time t into this chip (gw_fw_deliver) */
void gw_link_deliver(uint64_t t);
void gw_fw_deliver(int kind, int src_node, const uint32_t *q, int n);   /* src/gw_gmen.c */

#endif

// Copyright (c) 2022 Cesanta Software Limited
// All rights reserved
//
// This software is dual-licensed: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License version 3 as
// published by the Free Software Foundation. For the terms of this
// license, see http://www.fsf.org/licensing/licenses/agpl-3.0.html
//
// You are free to use this software under the terms of the GNU General
// Public License, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// Alternatively, you can license this software under a commercial
// license, please contact us at https://cesanta.com/contact.html

#include "mip.h"
#include <stdio.h>
#include <string.h>

#ifdef MIP_DEBUG
#define DBG(x) printf x
#else
#define DBG(x) \
  if (0) printf x
#endif

#define _packed __attribute__((packed))

struct lcp {
  uint8_t addr, ctrl, proto[2], code, id, len[2];
} _packed;

struct eth {
  uint8_t dst[6];  // Destination MAC address
  uint8_t src[6];  // Source MAC address
  uint16_t type;   // Ethernet type
} _packed;

struct ip {
  uint8_t ver;    // Version
  uint8_t tos;    // Unused
  uint16_t len;   // Length
  uint16_t id;    // Unused
  uint16_t frag;  // Fragmentation
  uint8_t ttl;    // Time to live
  uint8_t proto;  // Upper level protocol
  uint16_t csum;  // Checksum
  uint32_t src;   // Source IP
  uint32_t dst;   // Destination IP
} _packed;

struct ip6 {
  uint8_t ver;      // Version
  uint8_t opts[3];  // Options
  uint16_t len;     // Length
  uint8_t proto;    // Upper level protocol
  uint8_t ttl;      // Time to live
  uint8_t src[16];  // Source IP
  uint8_t dst[16];  // Destination IP
} _packed;

struct icmp {
  uint8_t type;
  uint8_t code;
  uint16_t csum;
} _packed;

struct arp {
  uint16_t fmt;    // Format of hardware address
  uint16_t pro;    // Format of protocol address
  uint8_t hlen;    // Length of hardware address
  uint8_t plen;    // Length of protocol address
  uint16_t op;     // Operation
  uint8_t sha[6];  // Sender hardware address
  uint32_t spa;    // Sender protocol address
  uint8_t tha[6];  // Target hardware address
  uint32_t tpa;    // Target protocol address
} _packed;

struct tcp {
  uint16_t sport;  // Source port
  uint16_t dport;  // Destination port
  uint32_t seq;    // Sequence number
  uint32_t ack;    // Acknowledgement number
  uint8_t off;     // Data offset
  uint8_t flags;   // TCP flags
  uint16_t win;    // Window
  uint16_t csum;   // Checksum
  uint16_t surp;   // Urgent pointer
} _packed;
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PUSH 0x08
#define TH_ACK 0x10
#define TH_URG 0x20
#define TH_ECE 0x40
#define TH_CWR 0x80
#define TH_FLAGS (TH_FIN | TH_SYN | TH_RST | TH_ACK | TH_URG | TH_ECE | TH_CWR)

struct udp {
  uint16_t sport;  // Source port
  uint16_t dport;  // Destination port
  uint16_t len;    // UDP length
  uint16_t csum;   // UDP checksum
} _packed;

struct dhcp {
  uint8_t op, htype, hlen, hops;
  uint32_t xid;
  uint16_t secs, flags;
  uint32_t ciaddr, yiaddr, siaddr, giaddr;
  uint8_t hwaddr[208];
  uint32_t magic;
  uint8_t options[32];
} _packed;

struct str {
  uint8_t *buf;
  size_t len;
};

struct pkt {
  struct str raw;  // Raw packet data
  struct str pay;  // Payload data
  struct eth *eth;
  struct llc *llc;
  struct arp *arp;
  struct ip *ip;
  struct ip6 *ip6;
  struct icmp *icmp;
  struct tcp *tcp;
  struct udp *udp;
  struct dhcp *dhcp;
};

#define U16(ptr) ((((uint16_t) (ptr)[0]) << 8) | (ptr)[1])
#define NET16(x) __builtin_bswap16(x)
#define NET32(x) __builtin_bswap32(x)
#define PDIFF(a, b) ((size_t) (((char *) (b)) - ((char *) (a))))

static struct str mkstr(void *buf, size_t len) {
  struct str str = {(uint8_t *) buf, len};
  return str;
}

static void mkpay(struct pkt *pkt, void *p) {
  pkt->pay = mkstr(p, (size_t) (&pkt->raw.buf[pkt->raw.len] - (uint8_t *) p));
}

static uint32_t csumup(uint32_t sum, const void *buf, size_t len) {
  const uint8_t *p = (const uint8_t *) buf;
  for (size_t i = 0; i < len; i++) sum += i & 1 ? p[i] : (uint32_t) (p[i] << 8);
  return sum;
}

static uint16_t csumfin(uint32_t sum) {
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return NET16(~sum & 0xffff);
}

static uint16_t ipcsum(const void *buf, size_t len) {
  uint32_t sum = csumup(0, buf, len);
  return csumfin(sum);
}

static void mip_call(struct mip_if *ifp, uint8_t ev, struct pkt *pkt) {
  if (ifp->ev != NULL) {
    struct mip_ev mev = {.ifp = ifp,
                         .event = ev,
                         .src_ip = pkt->ip ? pkt->ip->src : 0,
                         .dst_ip = pkt->ip ? pkt->ip->dst : 0,
                         .src_port = pkt->udp   ? pkt->udp->sport
                                     : pkt->tcp ? pkt->tcp->sport
                                                : 0,
                         .dst_port = pkt->udp   ? pkt->udp->dport
                                     : pkt->tcp ? pkt->tcp->dport
                                                : 0,
                         .buf = pkt->pay.buf,
                         .len = pkt->pay.len};
    ifp->ev(&mev);
  }
}

// ARP cache is organised as a doubly linked list. A successful cache lookup
// moves an entry to the head of the list. New entries are added by replacing
// the last entry in the list with a new IP/MAC.
// ARP cache format: | prev | next | Entry0 | Entry1 | .... | EntryN |
// ARP entry format: | prev | next | IP (4bytes) | MAC (6bytes) |
// prev and next are 1-byte offsets in the cache, so cache size is max 256 bytes
// ARP entry size is 12 bytes
static void arp_cache_init(uint8_t *p, int n, int size) {
  for (int i = 0; i < n; i++) p[2 + i * size] = (uint8_t) (2 + (i - 1) * size);
  for (int i = 0; i < n; i++) p[3 + i * size] = (uint8_t) (2 + (i + 1) * size);
  p[0] = p[2] = (uint8_t) (2 + (n - 1) * size);
  p[1] = p[3 + (n - 1) * size] = 2;
}

static uint8_t *arp_cache_find(struct mip_if *ifp, uint32_t ip) {
  uint8_t *p = ifp->arp_cache;
  if (ip == 0) return NULL;
  if (p[0] == 0 || p[1] == 0) arp_cache_init(p, MIP_ARP_ENTRIES, 12);
  for (uint8_t i = 0, j = p[1]; i < MIP_ARP_ENTRIES; i++, j = p[j + 1]) {
    if (memcmp(p + j + 2, &ip, sizeof(ip)) == 0) {
      p[1] = j, p[0] = p[j];  // Found entry! Point list head to us
      // DBG(("ARP find: %#lx @ %x:%x:%x:%x:%x:%x\n", (long) ip, p[j + 6],
      //     p[j + 7], p[j + 8], p[j + 9], p[j + 10], p[j + 11]));
      return p + j + 6;  // And return MAC address
    }
  }
  return NULL;
}

static void arp_cache_add(struct mip_if *ifp, uint32_t ip, uint8_t mac[6]) {
  uint8_t *p = ifp->arp_cache;
  if (ip == 0 || ip == ~0UL) return;            // Bad IP
  if (arp_cache_find(ifp, ip) != NULL) return;  // Already exists, do nothing
  memcpy(p + p[0] + 2, &ip, sizeof(ip));  // Replace last entry: IP address
  memcpy(p + p[0] + 6, mac, 6);           // And MAC address
  p[1] = p[0], p[0] = p[p[1]];            // Point list head to us
  // DBG(("ARP cache: added %#lx @ %x:%x:%x:%x:%x:%x\n", (long) ip, mac[0],
  //      mac[1], mac[2], mac[3], mac[4], mac[5]));
}

static struct ip *tx_ip(struct mip_if *ifp, uint8_t proto, uint32_t ip_src,
                        uint32_t ip_dst, size_t plen) {
  struct eth *eth = (struct eth *) ifp->buf;
  struct ip *ip = (struct ip *) (eth + 1);
  uint8_t *mac = arp_cache_find(ifp, ip_dst);         // Dst IP in ARP cache ?
  if (!mac) mac = arp_cache_find(ifp, ifp->gw);       // No, use gateway
  if (mac) memcpy(eth->dst, mac, sizeof(eth->dst));   // Found? Use it
  if (!mac) memset(eth->dst, 255, sizeof(eth->dst));  // No? Use broadcast
  memcpy(eth->src, ifp->mac, sizeof(eth->src));
  eth->type = NET16(0x800);
  ip->ver = 0x45;
  ip->tos = 0x0;
  ip->len = NET16((uint16_t) (sizeof(*ip) + plen));
  ip->id = 0;
  ip->frag = 0;
  ip->ttl = 255;
  ip->proto = proto;
  ip->src = ip_src;
  ip->dst = ip_dst;
  ip->csum = 0;
  ip->csum = ipcsum(ip, sizeof(*ip));
  return ip;
}

void mip_tx_udp(struct mip_if *ifp, uint32_t ip_src, uint16_t sport,
                uint32_t ip_dst, uint16_t dport, const void *buf, size_t len) {
  struct ip *ip = tx_ip(ifp, 17, ip_src, ip_dst, len + sizeof(struct udp));
  struct udp *udp = (struct udp *) (ip + 1);
  udp->sport = sport;
  udp->dport = dport;
  udp->len = NET16((uint16_t) (sizeof(*udp) + len));
  udp->csum = 0;
  uint32_t cs = csumup(0, udp, sizeof(*udp));
  cs = csumup(cs, buf, len);
  cs = csumup(cs, &ip->src, sizeof(ip->src));
  cs = csumup(cs, &ip->dst, sizeof(ip->dst));
  cs += ip->proto + sizeof(*udp) + len;
  udp->csum = csumfin(cs);
  memmove(udp + 1, buf, len);
  // DBG(("UDP LEN %d %d\n", (int) len, (int) ifp->frame_len));
  ifp->tx(ifp->buf, sizeof(struct eth) + sizeof(*ip) + sizeof(*udp) + len,
          ifp->txdata);
}

static void tx_dhcp(struct mip_if *ifp, uint32_t src, uint32_t dst,
                    uint8_t *opts, size_t optslen) {
  struct dhcp dhcp = {.op = 1,
                      .htype = 1,
                      .hlen = 6,
                      .magic = NET32(0x63825363),
                      .ciaddr = src};
  memcpy(&dhcp.hwaddr, ifp->mac, sizeof(ifp->mac));
  memcpy(&dhcp.xid, ifp->mac + 2, sizeof(dhcp.xid));
  memcpy(&dhcp.options, opts, optslen);
  mip_tx_udp(ifp, src, NET16(68), dst, NET16(67), &dhcp, sizeof(dhcp));
}

static void tx_dhcp_request(struct mip_if *ifp, uint32_t src, uint32_t dst) {
  uint8_t opts[] = {
      53, 1, 3,                 // Type: DHCP request
      55, 2, 1,   3,            // GW and mask
      12, 3, 'm', 'i', 'p',     // Host name: "mip"
      54, 4, 0,   0,   0,   0,  // DHCP server ID
      50, 4, 0,   0,   0,   0,  // Requested IP
      255                       // End of options
  };
  memcpy(opts + 14, &dst, sizeof(dst));
  memcpy(opts + 20, &src, sizeof(src));
  tx_dhcp(ifp, src, dst, opts, sizeof(opts));
}

static void tx_dhcp_discover(struct mip_if *ifp) {
  uint8_t opts[] = {
      53, 1, 1,     // Type: DHCP discover
      55, 2, 1, 3,  // Parameters: ip, mask
      255           // End of options
  };
  tx_dhcp(ifp, 0, 0xffffffff, opts, sizeof(opts));
}

static void rx_arp(struct mip_if *ifp, struct pkt *pkt) {
  // DBG(("ARP op %d %#x %#x\n", NET16(arp->op), arp->spa, arp->tpa));
  if (pkt->arp->op == NET16(1) && pkt->arp->tpa == ifp->ip) {
    // ARP request. Make a response, then send
    struct eth *eth = (struct eth *) ifp->buf;
    struct arp *arp = (struct arp *) (eth + 1);
    memcpy(eth->dst, pkt->eth->src, sizeof(eth->dst));
    memcpy(eth->src, ifp->mac, sizeof(eth->src));
    eth->type = NET16(0x806);
    *arp = *pkt->arp;
    arp->op = NET16(2);
    memcpy(arp->tha, pkt->arp->sha, sizeof(pkt->arp->tha));
    memcpy(arp->sha, ifp->mac, sizeof(pkt->arp->sha));
    arp->tpa = pkt->arp->spa;
    arp->spa = ifp->ip;
    DBG(("ARP response: we're %#lx\n", (long) ifp->ip));
    ifp->tx(ifp->buf, sizeof(*eth) + sizeof(*arp), ifp->txdata);
  } else if (pkt->arp->op == NET16(2)) {
    if (memcmp(pkt->arp->tha, ifp->mac, sizeof(pkt->arp->tha)) != 0) return;
    // arp_cache_add(ifp, pkt->arp->tpa, pkt->arp->tha);
  }
}

static void rx_icmp(struct mip_if *ifp, struct pkt *pkt) {
  // DBG(("ICMP %d\n", (int) len));
  mip_call(ifp, MIP_ICMP, pkt);
  if (pkt->icmp->type == 8 && pkt->ip->dst == ifp->ip) {
    struct ip *ip = tx_ip(ifp, 1, ifp->ip, pkt->ip->src,
                          sizeof(struct icmp) + pkt->pay.len);
    struct icmp *icmp = (struct icmp *) (ip + 1);
    memset(icmp, 0, sizeof(*icmp));  // Important - set csum to 0
    memcpy(icmp + 1, pkt->pay.buf, pkt->pay.len);
    icmp->csum = ipcsum(icmp, sizeof(*icmp) + pkt->pay.len);
    ifp->tx(ifp->buf, PDIFF(ifp->buf, icmp + 1) + pkt->pay.len, ifp->txdata);
  }
}

static void rx_dhcp(struct mip_if *ifp, struct pkt *pkt) {
  uint32_t ip = 0, gw = 0, mask = 0;
  uint8_t *p = pkt->dhcp->options, *end = &pkt->raw.buf[pkt->raw.len];
  if (end < (uint8_t *) (pkt->dhcp + 1)) return;
  // DBG(("DHCP %u\n", (unsigned) pkt->raw.len));
  while (p < end && p[0] != 255) {
    if (p[0] == 1 && p[1] == sizeof(ifp->mask)) {
      memcpy(&mask, p + 2, sizeof(mask));
      // DBG(("MASK %x\n", mask));
    } else if (p[0] == 3 && p[1] == sizeof(ifp->gw)) {
      memcpy(&gw, p + 2, sizeof(gw));
      ip = pkt->dhcp->yiaddr;
      // DBG(("IP %x GW %x\n", ip, gw));
    }
    p += p[1] + 2;
  }
  if (ip && mask && gw && ifp->ip == 0) {
    // DBG(("DHCP offer ip %#08lx mask %#08lx gw %#08lx\n",
    //      (long) ip, (long) mask, (long) gw));
    arp_cache_add(ifp, pkt->dhcp->siaddr, ((struct eth *) pkt->raw.buf)->src);
    ifp->ip = ip, ifp->gw = gw, ifp->mask = mask;
    struct mip_ev ev = {.ifp = ifp, .event = MIP_UP};
    if (ifp->ev) ifp->ev(&ev);
    ifp->up = 1;
    tx_dhcp_request(ifp, ip, pkt->dhcp->siaddr);
  }
}

static void rx_udp(struct mip_if *ifp, struct pkt *pkt) {
  mip_call(ifp, MIP_UDP, pkt);
}

static void rx_tcp(struct mip_if *ifp, struct pkt *pkt) {
  mip_call(ifp, MIP_TCP, pkt);
}

static void rx_ip(struct mip_if *ifp, struct pkt *pkt) {
  // DBG(("IP %d\n", (int) len));
  mip_call(ifp, MIP_IP, pkt);
  if (pkt->ip->proto == 1) {
    pkt->icmp = (struct icmp *) (pkt->ip + 1);
    if (pkt->pay.len < sizeof(*pkt->icmp)) return;
    mkpay(pkt, pkt->icmp + 1);
    rx_icmp(ifp, pkt);
  } else if (pkt->ip->proto == 17) {
    pkt->udp = (struct udp *) (pkt->ip + 1);
    if (pkt->pay.len < sizeof(*pkt->udp)) return;
    // DBG(("  UDP %u %u -> %u\n", len, NET16(udp->sport), NET16(udp->dport)));
    mkpay(pkt, pkt->udp + 1);
    if (pkt->udp->dport == NET16(68)) {
      pkt->dhcp = (struct dhcp *) (pkt->udp + 1);
      mkpay(pkt, pkt->dhcp + 1);
      rx_dhcp(ifp, pkt);
    } else {
      rx_udp(ifp, pkt);
    }
  } else if (pkt->ip->proto == 6) {
    pkt->tcp = (struct tcp *) (pkt->ip + 1);
    if (pkt->pay.len < sizeof(*pkt->tcp)) return;
    mkpay(pkt, pkt->tcp + 1);
    rx_tcp(ifp, pkt);
  }
}

static void rx_ip6(struct mip_if *ifp, struct pkt *pkt) {
  // DBG(("IP %d\n", (int) len));
  mip_call(ifp, MIP_IP, pkt);
  if (pkt->ip6->proto == 1 || pkt->ip6->proto == 58) {
    pkt->icmp = (struct icmp *) (pkt->ip6 + 1);
    if (pkt->pay.len < sizeof(*pkt->icmp)) return;
    mkpay(pkt, pkt->icmp + 1);
    rx_icmp(ifp, pkt);
  } else if (pkt->ip->proto == 17) {
    pkt->udp = (struct udp *) (pkt->ip6 + 1);
    if (pkt->pay.len < sizeof(*pkt->udp)) return;
    // DBG(("  UDP %u %u -> %u\n", len, NET16(udp->sport), NET16(udp->dport)));
    mkpay(pkt, pkt->udp + 1);
  }
}

void mip_rx(struct mip_if *ifp, void *buf, size_t len) {
  // DBG(("gt frame %u bytes\n", len));
  const uint8_t broadcast[] = {255, 255, 255, 255, 255, 255};
  struct pkt pkt = {.raw = {.buf = (uint8_t *) buf, .len = len}};
  pkt.eth = (struct eth *) buf;
  if (pkt.raw.len < sizeof(*pkt.eth)) return;  // Truncated - runt?
  if (memcmp(pkt.eth->dst, ifp->mac, sizeof(pkt.eth->dst)) != 0 &&
      memcmp(pkt.eth->dst, broadcast, sizeof(pkt.eth->dst)) != 0) {
    // Not for us. Drop silently
  } else if (pkt.eth->type == NET16(0x806)) {
    pkt.arp = (struct arp *) (pkt.eth + 1);
    if (sizeof(*pkt.eth) + sizeof(*pkt.arp) > pkt.raw.len) return;  // Truncated
    rx_arp(ifp, &pkt);
  } else if (pkt.eth->type == NET16(0x86dd)) {
    pkt.ip6 = (struct ip6 *) (pkt.eth + 1);
    if (pkt.raw.len < sizeof(*pkt.eth) + sizeof(*pkt.ip6)) return;  // Truncated
    if ((pkt.ip6->ver >> 4) != 0x6) return;                         // Not IP
    mkpay(&pkt, pkt.ip6 + 1);
    rx_ip6(ifp, &pkt);
  } else if (pkt.eth->type == NET16(0x800)) {
    pkt.ip = (struct ip *) (pkt.eth + 1);
    if (pkt.raw.len < sizeof(*pkt.eth) + sizeof(*pkt.ip)) return;  // Truncated
    if ((pkt.ip->ver >> 4) != 4) return;                           // Not IP
    mkpay(&pkt, pkt.ip + 1);
    rx_ip(ifp, &pkt);
  } else {
    DBG(("  Unknown eth type %x\n", NET16(pkt.eth->type)));
  }
}

void mip_poll(struct mip_if *ifp, uint64_t uptime_ms) {
  // DBG(("poll: %p %lld\n", ifp, uptime_ms));

  // Send DHCP If required
  if (ifp->ip == 0 && ifp->use_dhcp && uptime_ms > ifp->timer) {
    tx_dhcp_discover(ifp);
    ifp->timer = uptime_ms + 3000;
  }

  // Handle physical interface up/down status
  if (ifp->phy) {
    bool up = ifp->phy();
    if (up != ifp->up) {
      if (!up && ifp->ev) {
        struct mip_ev ev = {.ifp = ifp, .event = MIP_DOWN};
        ifp->ev(&ev);
        if (ifp->use_dhcp) ifp->ip = 0;
      }
      if (up && ifp->ev && ifp->use_dhcp == false) {
        struct mip_ev ev = {.ifp = ifp, .event = MIP_UP};
        ifp->ev(&ev);
      }
      ifp->up = up;
    }
  }

  // Send POLL event
  struct mip_ev ev = {.ifp = ifp, .event = MIP_POLL};
  if (ifp->ev) ifp->ev(&ev);
}

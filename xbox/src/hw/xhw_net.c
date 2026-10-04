/* xhw_net.c - the network: nxdk's lwIP 2.2.1 and NIC driver (nvnetdrv),
 * linked from libnxdk_net.lib and driven directly (docs/lan-plan.md D3, D11,
 * D14; docs/platform.md "Network").
 *
 * Nothing here runs until xhw_net_start: an offline boot never touches the
 * NIC (D14 rule 1), and the library costs only its code. xhw_net_start
 * returns at once; a worker starts lwIP's thread (tcpip_init, priority
 * raised to +1), adds the NIC (nvnetif_init: 64 receive buffers of 2 KB in
 * contiguous memory, the interrupt connected), then takes an address: the
 * dashboard's manual address if the configuration sector has one (after an
 * ACD check of our own, RFC 5227: lwIP checks only the addresses it picks),
 * else DHCP, and AutoIP (169.254.1.0-169.254.254.255) when no lease has come
 * 4 s after the link came up, while DHCP keeps trying (a lease that comes
 * replaces the AutoIP address). nxNetInit is not used: it blocks for up to
 * 10 s and has no AutoIP.
 *
 * lwIP's own ACD acts on conflicts (a DHCP address declined, a link-local
 * one replaced, an address in use defended once and then given up, RFC 5227
 * §2.4 b); the worker watches for them and reports XHW_NET_CONFLICT while
 * one is recent. The link is polled (nvnetdrv_is_link_up, four times a
 * second): the library's link callback is the driver's own, and lwIP's netif
 * callback is not compiled in; a change seen by the poll is handed to lwIP
 * as well (netif_set_link_up does dhcp_network_changed, INIT-REBOOT, and
 * does nothing when the driver already did).
 *
 * IPv4 only (D14 rule 6): the NIC's frames reach lwIP through net_input,
 * which drops IPv6 and multicast frames. Without it a router advertisement
 * would make lwIP send a router solicitation (nd6.c) though the netif has no
 * IPv6 address.
 *
 * UDP goes through lwIP's raw API under the core lock, TTL 1 on every
 * socket. Received datagrams pass the source filters (conduct_rx_ok) and
 * are copied on lwIP's thread into a ring per socket in the XBE's memory,
 * taken out by xhw_udp_recv; sends pass the destination rules
 * (conduct_tx_ok) and the socket's governor (gov_take). lwIP's thread and
 * the NIC's DPC never touch the game's lazily committed memory (MEM1,
 * ARAM). No lwIP type crosses xhw.h. xhw_net_shutdown stops the NIC before
 * every XLaunchXBE (xhw_main.c): a device left running across a relaunch
 * writes into the next image. The lease is not released at exit (RFC 2131
 * allows it; the dashboard takes the same address again). */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/acd.h>
#include <lwip/autoip.h>
#include <lwip/dhcp.h>
#include <lwip/init.h>
#include <lwip/ip.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/pbuf.h>
#include <lwip/prot/dhcp.h>
#include <lwip/tcpip.h>
#include <lwip/udp.h>
#include <nvnetdrv.h>
#include <nxdk/configsector.h>

#include "xhw.h"
#include "xhw_internal.h"

err_t nvnetif_init(struct netif* netif);   /* nvnetdrv_lwip.c, in libnxdk_net.lib */

/* ---- datagram ring (tools/xbox/test_net_ring.py cuts this part out) ----
 * One writer and one reader, no lock: head (bytes written) is stored only
 * by the writer, tail (bytes consumed) only by the reader, and both only
 * grow. A record is an 8-byte header and the payload rounded up to 8 bytes;
 * one that doesn't fit before the ring's end leaves a skip header there and
 * starts again at offset 0. A record that doesn't fit at all is dropped and
 * counted (the newest goes, what is queued stays in order). */
typedef struct {
    uint16_t len;   /* payload bytes, or RING_SKIP: the rest up to the end is unused */
    uint16_t port;
    uint32_t ip;
} RingHdr;
#define RING_SKIP 0xFFFFu

typedef struct {
    uint8_t* buf;
    uint32_t size;               /* a power of two, at least 64 */
    volatile uint32_t head;      /* the writer's */
    volatile uint32_t tail;      /* the reader's */
    volatile uint32_t dropped;   /* the writer's: records that didn't fit */
} Ring;

static void ring_init(Ring* r, void* buf, uint32_t size) {
    r->buf = (uint8_t*)buf;
    r->size = size;
    r->head = r->tail = r->dropped = 0;
}

static uint32_t ring_need(uint32_t n) { return 8u + ((n + 7u) & ~7u); }

/* The largest payload a ring takes: a record of up to half the ring always
 * fits into an empty one, wherever the skip falls. */
static uint32_t ring_max(const Ring* r) { return r->size / 2 - 8u < 0xFFFEu ? r->size / 2 - 8u : 0xFFFEu; }

/* Writer: 1 queued, 0 dropped. */
static int ring_push(Ring* r, const void* p, uint32_t n, uint32_t ip, uint16_t port) {
    uint32_t head = r->head, tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t off = head & (r->size - 1), need = ring_need(n), skip = 0;
    RingHdr* h;
    if (n > ring_max(r)) {
        r->dropped++;
        return 0;
    }
    if (r->size - off < need) skip = r->size - off;   /* a multiple of 8, so a header fits */
    if (head - tail + skip + need > r->size) {
        r->dropped++;
        return 0;
    }
    if (skip) {
        ((RingHdr*)(r->buf + off))->len = RING_SKIP;
        head += skip;
        off = 0;
    }
    h = (RingHdr*)(r->buf + off);
    h->len = (uint16_t)n;
    h->port = port;
    h->ip = ip;
    memcpy(r->buf + off + 8, p, n);
    __atomic_store_n(&r->head, head + need, __ATOMIC_RELEASE);   /* the skip and the record at once */
    return 1;
}

/* Reader: the payload's length (at most cap bytes copied; a longer one is
 * cut, as recvfrom does), -1 when the ring is empty. */
static int ring_pop(Ring* r, void* p, uint32_t cap, uint32_t* ip, uint16_t* port) {
    uint32_t tail = r->tail, head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    for (;;) {
        uint32_t off, n;
        const RingHdr* h;
        if (tail == head) return -1;
        off = tail & (r->size - 1);
        h = (const RingHdr*)(r->buf + off);
        if (h->len == RING_SKIP) {
            tail += r->size - off;
            continue;
        }
        n = h->len;
        if (ip) *ip = h->ip;
        if (port) *port = h->port;
        memcpy(p, r->buf + off + 8, n < cap ? n : cap);
        __atomic_store_n(&r->tail, tail + ring_need(n), __ATOMIC_RELEASE);
        return (int)(n < cap ? n : cap);
    }
}

static int ring_empty(const Ring* r) { return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) == r->tail; }
/* ---- end of the datagram ring ---- */

/* ---- conduct: filters and the governor (tools/xbox/test_net_gov.py cuts this part out) ----
 * D14 rules 7-10 as pure functions of addresses, sizes and time, so a host
 * test can hold them against a model. Addresses in network order (a.b.c.d
 * is a | b << 8 | c << 16 | d << 24), so the first octet is ip & 255. */
#define NET_MAX_SEND 1200u          /* XHW_UDP_MAX_SEND */
#define NET_DISCOVERY 41001u        /* XHW_NET_DISCOVERY_PORT */
#define NET_BCAST_MAX 200u          /* a broadcast datagram is shorter than this */
#define GOV_RATE 250u               /* datagrams a second */
#define GOV_BYTES 262144u           /* bytes a second */
#define GOV_BURST 32u               /* the bucket: 32 datagrams, 32 full-size ones' bytes */
#define GOV_ONE 1000000ull          /* one token, in the buckets' units (a millionth) */

enum { TX_OK, TX_SIZE, TX_NOADDR, TX_PORT, TX_DEST, TX_OFFLINK, TX_BCAST_PORT, TX_BCAST_SIZE, TX_BCAST_RATE, TX_GOV,
       TX_N };
enum { RX_OK, RX_SELF, RX_ZERO, RX_BCAST, RX_MCAST, RX_PORT, RX_OFFLINK, RX_N };

static int ip_link_local(uint32_t ip) { return (ip & 0xFFFFu) == (169u | 254u << 8); }

/* The subnet a peer must share with us: our mask, or 169.254/16 while our
 * address is link-local (whatever mask came with it). */
static uint32_t on_link_mask(uint32_t me, uint32_t mask) { return ip_link_local(me) ? 0xFFFFu : mask; }

/* A received datagram's source: RX_OK or why it is dropped (D14 rule 9). */
static int conduct_rx(uint32_t src, uint16_t sport, uint32_t me, uint32_t mask) {
    uint32_t m = on_link_mask(me, mask), first = src & 255u;
    if (src == me) return RX_SELF;
    if (first == 0) return RX_ZERO;   /* 0.0.0.0/8: "this host" */
    if (src == 0xFFFFFFFFu || (m != 0xFFFFFFFFu && (src & ~m) == ~m)) return RX_BCAST;
    if (first >= 224) return RX_MCAST;   /* multicast and the reserved class E */
    if (sport == 0) return RX_PORT;
    if (!me || (src & m) != (me & m) || first == 127) return RX_OFFLINK;
    return RX_OK;
}

/* A datagram we would send: TX_OK or why it is refused (rules 7-9; the
 * broadcast window and the governor are separate, below). */
static int conduct_tx(uint32_t dst, uint16_t dport, uint32_t n, uint32_t me, uint32_t mask) {
    uint32_t m = on_link_mask(me, mask), first = dst & 255u;
    if (n > NET_MAX_SEND) return TX_SIZE;
    if (!me) return TX_NOADDR;
    if (dport == 0) return TX_PORT;
    if (dst == 0xFFFFFFFFu) return dport != NET_DISCOVERY ? TX_BCAST_PORT : n >= NET_BCAST_MAX ? TX_BCAST_SIZE : TX_OK;
    if (dst == me || first == 0 || first == 127 || first >= 224) return TX_DEST;
    if ((dst & m) != (me & m)) return TX_OFFLINK;
    if (m != 0xFFFFFFFFu && (dst & ~m) == ~m) return TX_DEST;   /* the directed broadcast (rule 8) */
    return TX_OK;
}

/* The transmit ceiling of one socket (rule 10): two token buckets, refilled
 * by the microsecond, that a datagram passes only if both have room; and
 * the broadcast window, at most two in any second. */
typedef struct {
    uint64_t last_us;
    uint64_t pk, by;         /* tokens, in millionths of a datagram and of a byte */
    uint64_t bc[2];          /* the last two broadcasts' times, oldest first */
    uint32_t bc_n;
} Gov;

static void gov_init(Gov* g, uint64_t now_us) {
    g->last_us = now_us;
    g->pk = GOV_BURST * GOV_ONE;
    g->by = (uint64_t)GOV_BURST * NET_MAX_SEND * GOV_ONE;
    g->bc[0] = g->bc[1] = 0;
    g->bc_n = 0;
}

static int gov_take(Gov* g, uint64_t now_us, uint32_t n) {
    uint64_t dt = now_us > g->last_us ? now_us - g->last_us : 0;
    g->last_us = now_us > g->last_us ? now_us : g->last_us;
    if (dt > 10000000u) dt = 10000000u;   /* the buckets are full long before */
    g->pk += dt * GOV_RATE;
    g->by += dt * GOV_BYTES;
    if (g->pk > GOV_BURST * GOV_ONE) g->pk = GOV_BURST * GOV_ONE;
    if (g->by > (uint64_t)GOV_BURST * NET_MAX_SEND * GOV_ONE) g->by = (uint64_t)GOV_BURST * NET_MAX_SEND * GOV_ONE;
    if (g->pk < GOV_ONE || g->by < (uint64_t)n * GOV_ONE) return 0;
    g->pk -= GOV_ONE;
    g->by -= (uint64_t)n * GOV_ONE;
    return 1;
}

/* A broadcast at now_us keeps "at most two in any one-second window": the
 * one two before must be more than a second old. */
static int gov_bcast(Gov* g, uint64_t now_us) {
    if (g->bc_n == 2 && now_us - g->bc[0] <= 1000000u) return 0;
    if (g->bc_n == 2) g->bc[0] = g->bc[1];
    g->bc[g->bc_n == 2 ? 1 : g->bc_n] = now_us;
    if (g->bc_n < 2) g->bc_n++;
    return 1;
}
/* ---- end of conduct ---- */

/* ---- state ---- */
static volatile LONG s_started;
static volatile int s_state = XHW_NET_OFF;
static volatile int s_ready;     /* lwIP's thread runs and the NIC is added: sockets work */
static volatile int s_nic_on;    /* nvnetdrv_init succeeded: shutdown stops it */
static volatile int s_shut, s_paused;
static volatile int s_how;
static volatile uint32_t s_ip, s_mask, s_bcast, s_up_ms, s_link_ms;
static volatile uint32_t s_in_dropped;   /* frames net_input kept from lwIP (IPv6, multicast) */
static uint64_t s_t0;
static struct netif s_netif;
static HANDLE s_init_ev;

static uint64_t now_us(void) { return xhw_time_ns() / 1000u; }
static uint32_t ms_since_start(void) { return (uint32_t)((xhw_time_ns() - s_t0) / 1000000u); }

static void fmt_ip(char* out, size_t cap, uint32_t ip) {
    snprintf(out, cap, "%u.%u.%u.%u", ip & 255u, ip >> 8 & 255u, ip >> 16 & 255u, ip >> 24 & 255u);
}

/* On lwIP's thread, once it runs: its priority, as the timer thread's
 * (sys_thread_new ignores the one lwIP asks for). Small stack: no logging. */
static void tcpip_ready(void* arg) {
    (void)arg;
    KeSetBasePriorityThread(KeGetCurrentThread(), 1);
    SetEvent(s_init_ev);
}

/* The NIC's frames on their way to lwIP (the DPC, via the driver's
 * rx_callback): IPv6 and multicast frames go no further (D14 rule 6). */
static err_t net_input(struct pbuf* p, struct netif* inp) {
    const uint8_t* f = (const uint8_t*)p->payload;
    if (p->len >= 14) {
        int ipv6 = f[12] == 0x86 && f[13] == 0xDD;
        int mcast = (f[0] & 1) && !(f[0] == 0xFF && f[1] == 0xFF && f[2] == 0xFF && f[3] == 0xFF && f[4] == 0xFF &&
                                    f[5] == 0xFF);
        if (ipv6 || mcast) {
            s_in_dropped++;
            pbuf_free(p);
            return ERR_OK;
        }
    }
    return tcpip_input(p, inp);
}

/* ---- sockets ---- */
#define SOCK_RING (64u * 1024)
#define RX_MAX 2048u   /* the largest datagram kept (lwIP reassembles fragments) */
static const char* const k_tx_why[TX_N] = { "ok", "over 1200 bytes", "no address", "port 0", "not a peer's address",
                                            "off the link", "broadcast to another port", "broadcast of 200 bytes or more",
                                            "a third broadcast within a second", "governor" };
static const char* const k_rx_why[RX_N] = { "ok", "our own address", "0.0.0.0/8", "a broadcast address", "multicast",
                                            "port 0", "off the link" };
typedef struct {
    struct udp_pcb* pcb;
    Ring ring;
    HANDLE ev;                     /* auto-reset: a datagram was queued */
    volatile uint32_t toolong;     /* datagrams over RX_MAX */
    volatile uint32_t rx_why[RX_N];   /* lwIP's thread: datagrams the source filter dropped */
    volatile uint32_t rx_last_ip;     /* the last one's source, for the log */
    volatile uint16_t rx_last_port;
    uint32_t rx_logged, dropped_logged;
    uint32_t tx_why[TX_N];         /* refused sends, by reason */
    uint32_t tx_logged;
    uint64_t tx_log_us;
    Gov gov;
    uint16_t port;
    int used;                      /* the slot is taken (set under the core lock) */
    int quiet;                     /* the log stream's own socket: its counters are not logged (they would feed it) */
} Sock;
static Sock s_sock[XHW_UDP_MAX];

/* lwIP's thread, core lock held: filter, copy, let go of the pbuf at once */
static void on_udp(void* arg, struct udp_pcb* pcb, struct pbuf* p, const ip_addr_t* addr, u16_t port) {
    static uint8_t tmp[RX_MAX];   /* lwIP's thread only; its stack is 4 KB */
    Sock* s = (Sock*)arg;
    uint32_t src = ip_2_ip4(addr)->addr;
    int why = conduct_rx(src, port, s_ip, s_mask);
    (void)pcb;
    if (why != RX_OK) {
        s->rx_why[why]++;
        s->rx_last_ip = src;
        s->rx_last_port = port;
    } else if (p->tot_len <= RX_MAX) {
        uint16_t n = pbuf_copy_partial(p, tmp, p->tot_len, 0);
        if (ring_push(&s->ring, tmp, n, src, port)) SetEvent(s->ev);
    } else {
        s->toolong++;
    }
    pbuf_free(p);
}

static Sock* sock_get(int h) { return h >= 0 && h < XHW_UDP_MAX && s_sock[h].pcb ? &s_sock[h] : NULL; }

int xhw_udp_open(uint16_t port) {
    int h;
    Sock* s = NULL;
    void* buf;
    struct udp_pcb* pcb;
    if (!s_ready || s_shut) return -1;
    LOCK_TCPIP_CORE();
    for (h = 0; h < XHW_UDP_MAX; h++)
        if (!s_sock[h].used) {
            s = &s_sock[h];
            s->used = 1;
            break;
        }
    UNLOCK_TCPIP_CORE();
    if (!s) return -1;
    if (!(buf = malloc(SOCK_RING))) {
        s->used = 0;
        return -1;
    }
    if (!s->ev) s->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ring_init(&s->ring, buf, SOCK_RING);
    memset((void*)s->rx_why, 0, sizeof s->rx_why);
    memset(s->tx_why, 0, sizeof s->tx_why);
    s->toolong = s->dropped_logged = s->rx_logged = s->tx_logged = 0;
    s->tx_log_us = 0;
    s->quiet = 0;
    gov_init(&s->gov, now_us());
    LOCK_TCPIP_CORE();
    pcb = udp_new();
    if (pcb) {
        pcb->ttl = 1;   /* D14 rule 7: link-local by design, dies at the first router */
        ip_set_option(pcb, SOF_BROADCAST);
        if (udp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK) {
            udp_remove(pcb);
            pcb = NULL;
        } else {
            udp_recv(pcb, on_udp, s);
            s->port = pcb->local_port;
            s->pcb = pcb;
        }
    }
    UNLOCK_TCPIP_CORE();
    if (!pcb) {
        free(buf);
        s->used = 0;
        xhw_logf("[NET] udp port %u: could not open", (unsigned)port);
        return -1;
    }
    xhw_logf("[NET] udp %d open on port %u", h, (unsigned)s->port);
    return h;
}

/* The counters of refused sends and filtered receives, as [NET] lines: at
 * most one of each a second per socket, on the caller's thread (never on
 * lwIP's). */
static void sock_report(Sock* s, int h, int force) {
    uint64_t t = now_us();
    uint32_t i, tx = 0, rx = 0;
    char line[300];
    int len;
    for (i = 1; i < TX_N; i++) tx += s->tx_why[i];
    for (i = 1; i < RX_N; i++) rx += s->rx_why[i];
    if (s->quiet || (tx == s->tx_logged && rx == s->rx_logged) || (!force && t - s->tx_log_us < 1000000u)) return;
    s->tx_log_us = t;
    if (tx != s->tx_logged) {
        s->tx_logged = tx;
        if (s->tx_why[TX_GOV]) xhw_logf("[NET] tx governor dropped %u (udp %d)", (unsigned)s->tx_why[TX_GOV], h);
        len = snprintf(line, sizeof line, "[NET] udp %d tx refused so far:", h);
        for (i = 1; i < TX_N && len < (int)sizeof line - 48; i++)
            if (i != TX_GOV && s->tx_why[i]) len += snprintf(line + len, sizeof line - len, " %u %s,", (unsigned)s->tx_why[i], k_tx_why[i]);
        if (tx != s->tx_why[TX_GOV]) xhw_log(line);
    }
    if (rx != s->rx_logged) {
        s->rx_logged = rx;
        len = snprintf(line, sizeof line, "[NET] udp %d rx filtered so far:", h);
        for (i = 1; i < RX_N && len < (int)sizeof line - 48; i++)
            if (s->rx_why[i]) len += snprintf(line + len, sizeof line - len, " %u %s,", (unsigned)s->rx_why[i], k_rx_why[i]);
        {
            uint32_t a = s->rx_last_ip;
            snprintf(line + len, sizeof line - len, " the last from %u.%u.%u.%u:%u", a & 255u, a >> 8 & 255u, a >> 16 & 255u,
                     a >> 24, (unsigned)s->rx_last_port);
        }
        xhw_log(line);
    }
}

void xhw_udp_close(int h) {
    Sock* s = sock_get(h);
    void* buf;
    if (!s) return;
    sock_report(s, h, 1);
    LOCK_TCPIP_CORE();   /* no on_udp after this: it runs under the same lock */
    udp_remove(s->pcb);
    s->pcb = NULL;
    buf = s->ring.buf;
    s->ring.buf = NULL;
    s->used = 0;
    UNLOCK_TCPIP_CORE();
    if (s->ring.dropped || s->toolong)
        xhw_logf("[NET] udp %d closed: %u dropped (ring full), %u too long", h, (unsigned)s->ring.dropped,
                 (unsigned)s->toolong);
    free(buf);
}

int xhw_udp_send(int h, uint32_t ip, uint16_t port, const void* p, uint32_t n) {
    Sock* s = sock_get(h);
    struct pbuf* pb;
    err_t err = ERR_IF;
    uint64_t t;
    int why;
    if (!s || s_paused || s_shut) return 0;
    t = now_us();
    why = conduct_tx(ip, port, n, xhw_net_ip(), s_mask);
    if (why == TX_OK && ip == 0xFFFFFFFFu && !gov_bcast(&s->gov, t)) why = TX_BCAST_RATE;
    if (why == TX_OK && !gov_take(&s->gov, t, n)) why = TX_GOV;
    if (why != TX_OK) {
        s->tx_why[why]++;
        sock_report(s, h, 0);
        return 0;
    }
    LOCK_TCPIP_CORE();
    pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
    if (pb) {
        ip_addr_t dst;
        memcpy(pb->payload, p, n);
        ip_addr_set_ip4_u32(&dst, ip);
        err = udp_sendto(s->pcb, pb, &dst, port);
        pbuf_free(pb);
    }
    UNLOCK_TCPIP_CORE();
    return err == ERR_OK;
}

int xhw_udp_recv(int h, void* p, uint32_t cap, uint32_t* ip, uint16_t* port) {
    Sock* s = sock_get(h);
    int n;
    if (!s) return -1;
    n = ring_pop(&s->ring, p, cap, ip, port);
    if (s->ring.dropped != s->dropped_logged) {   /* on the reader: the writer must not log */
        s->dropped_logged = s->ring.dropped;
        xhw_logf("[NET] udp %d: %u datagrams dropped so far (ring full)", h, (unsigned)s->dropped_logged);
    }
    sock_report(s, h, 0);
    return n;
}

int xhw_udp_wait(int h, uint32_t ms) {
    Sock* s = sock_get(h);
    if (!s) return 0;
    if (!ring_empty(&s->ring)) return 1;
    WaitForSingleObject(s->ev, ms);
    return !ring_empty(&s->ring);
}

/* ---- the manual address's ACD check (RFC 5227; lwIP checks only its own) ---- */
static struct acd s_manual_acd;
static ip4_addr_t s_manual_ip, s_manual_mask, s_manual_gw;
static volatile int s_manual;          /* 0 none, 1 probing, 2 in use, 3 conflict, 4 conflict (logged) */

/* lwIP's thread, core locked. A conflict (ACD_DECLINE, and the
 * ACD_RESTART_CLIENT that follows it at once or after lwIP's rate limit)
 * leaves the console without an address: a manual address has no other to
 * try, and probing it again at once would only repeat the conflict. It is
 * checked again at the next link up. */
static void manual_acd_cb(struct netif* netif, acd_callback_enum_t st) {
    if (st == ACD_IP_OK) {
        netif_set_addr(netif, &s_manual_ip, &s_manual_mask, &s_manual_gw);
        s_manual = 2;
    } else {
        netif_set_addr(netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4);
        if (s_manual < 3) s_manual = 3;
    }
}

/* ---- bring-up and the address ---- */
static const char* const k_how[] = { "none", "manual", "dhcp", "autoip" };
static const char* const k_state[] = { "off", "no cable", "getting an address", "up", "failed", "address conflict" };

static void set_state(int st) {
    if (st == s_state) return;
    s_state = st;
    xhw_logf("[NET] state: %s after %u ms", k_state[st], ms_since_start());
}

static void net_worker(void* arg) {
    ip4_addr_t any;
    nxdk_network_config_sector_t cfg;
    int manual = 0, link = -1, autoip_on = 0, dhcp_backoff = 0;
    uint32_t free0 = xhw_mem_free_kb(), link_since = 0, last_ip = 0, tried = 0, conflict_until = 0;
    const uint8_t* mac;
    err_t err;
    (void)arg;
    xhw_logf("[NET] starting lwIP %s, free %u KB", LWIP_VERSION_STRING, (unsigned)free0);
    ip4_addr_set_zero(&any);
    if (nxLoadNetworkConfig(&cfg) && (cfg.dhcpFlags & NXDK_NETWORK_CONFIG_MANUAL_IP)) {
        /* the dashboard's manual address; the sector holds it in network order */
        manual = 1;
        s_manual_ip.addr = cfg.manual.ip;
        s_manual_mask.addr = cfg.manual.subnetMask;
        s_manual_gw.addr = cfg.manual.defaultGateway;
    }
    s_init_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    tcpip_init(tcpip_ready, NULL);
    if (WaitForSingleObject(s_init_ev, 5000) != WAIT_OBJECT_0) {
        xhw_logf("[NET] lwIP's thread did not start");
        set_state(XHW_NET_FAILED);
        return;
    }
    if (s_shut) return;
    /* no address yet in any case: a manual one only after its ACD check */
    err = netifapi_netif_add(&s_netif, &any, &any, &any, NULL, nvnetif_init, net_input);
    if (err != ERR_OK) {
        xhw_logf("[NET] the NIC did not start (lwIP error %d)", (int)err);
        set_state(XHW_NET_FAILED);
        return;
    }
    s_nic_on = 1;
    LOCK_TCPIP_CORE();
    netif_set_ip6_autoconfig_enabled(&s_netif, 0);   /* IPv4 only (D14 rule 6); net_input drops IPv6 anyway */
    UNLOCK_TCPIP_CORE();
    netifapi_netif_set_default(&s_netif);
    netifapi_netif_set_up(&s_netif);
    mac = nvnetdrv_get_ethernet_addr();
    xhw_logf("[NET] NIC up after %u ms: MAC %02x:%02x:%02x:%02x:%02x:%02x, free %u KB (%d KB for the stack)",
             ms_since_start(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)xhw_mem_free_kb(),
             (int)free0 - (int)xhw_mem_free_kb());
    if (manual) {
        char a[16];
        fmt_ip(a, sizeof a, s_manual_ip.addr);
        xhw_logf("[NET] manual address %s from the dashboard: checking it (ACD)", a);
        LOCK_TCPIP_CORE();
        acd_add(&s_netif, &s_manual_acd, manual_acd_cb);
        UNLOCK_TCPIP_CORE();
    } else {
        netifapi_dhcp_start(&s_netif);
    }
    s_ready = 1;
    while (!s_shut) {
        int up, how, defending = 0, newly = 0, no_offer = 1;
        uint32_t cur, cur_mask, now;
        Sleep(250);
        if (s_shut) break;
        if (s_paused) continue;
        now = ms_since_start();
        up = nvnetdrv_is_link_up();
        if (up != link) {
            link = up;
            if (up) {
                link_since = now;
                if (!s_link_ms) s_link_ms = now ? now : 1;
                netifapi_netif_set_link_up(&s_netif);   /* DHCP INIT-REBOOT, AutoIP and ACD restart (lwIP) */
            } else {
                netifapi_netif_set_link_down(&s_netif);
            }
            xhw_logf("[NET] link %s after %u ms", up ? "up" : "down", now);
            if (up && manual) {   /* (re)check the manual address on every link up */
                LOCK_TCPIP_CORE();
                acd_start(&s_netif, &s_manual_acd, s_manual_ip);
                UNLOCK_TCPIP_CORE();
                s_manual = 1;
            }
        }
        LOCK_TCPIP_CORE();
        {
            struct dhcp* d = netif_dhcp_data(&s_netif);
            struct autoip* ai = netif_autoip_data(&s_netif);
            int leased = !manual && dhcp_supplied_address(&s_netif), backoff;
            if (leased && netif_ip4_addr(&s_netif)->addr != d->offered_ip_addr.addr) {
                /* AutoIP finished its check after the lease came and bound over it
                 * (lwIP 2.2.1's autoip_stop leaves its ACD running): the lease wins */
                if (ai) acd_stop(&ai->acd);
                autoip_stop(&s_netif);
                autoip_on = 0;
                netif_set_addr(&s_netif, &d->offered_ip_addr, &d->offered_sn_mask, &d->offered_gw_addr);
                newly = -1;
            }
            cur = netif_ip4_addr(&s_netif)->addr;
            cur_mask = netif_ip4_netmask(&s_netif)->addr;
            how = manual ? (s_manual == 2 ? XHW_NET_HOW_MANUAL : XHW_NET_HOW_NONE)
                  : leased ? XHW_NET_HOW_DHCP
                  : autoip_supplied_address(&s_netif) ? XHW_NET_HOW_AUTOIP
                                                      : XHW_NET_HOW_NONE;
            /* no offer in hand (an offer under its ACD check is a lease on its way) */
            no_offer = !d || d->state == DHCP_STATE_INIT || d->state == DHCP_STATE_SELECTING ||
                       d->state == DHCP_STATE_REBOOTING || d->state == DHCP_STATE_BACKING_OFF;
            if (autoip_on && leased) {   /* a lease after all: it has replaced the AutoIP address */
                if (ai) acd_stop(&ai->acd);   /* else its check, still running, binds over the lease */
                autoip_stop(&s_netif);
                autoip_on = 0;
            }
            /* what lwIP's ACD did since the last look */
            backoff = d && d->state == DHCP_STATE_BACKING_OFF;
            if (backoff && !dhcp_backoff) newly = 1;   /* the offered address was in use: declined */
            dhcp_backoff = backoff;
            if (ai && ai->tried_llipaddr != tried) {   /* one more on each conflict (autoip_restart) */
                tried = ai->tried_llipaddr;
                newly = 2;   /* the link-local address is in use: lwIP picks another */
            }
            defending = (d && d->acd.lastconflict) || (ai && ai->acd.lastconflict) || (manual && s_manual_acd.lastconflict);
        }
        UNLOCK_TCPIP_CORE();
        if (manual && s_manual == 3) newly = newly ? newly : 3;
        if (newly == -1) {
            xhw_logf("[NET] AutoIP bound over the DHCP lease: the lease's address put back");
            newly = 0;
        }
        if (newly || defending) {
            if (newly || now >= conflict_until)
                xhw_logf("[NET] address conflict (%s)", newly == 1 ? "DHCP's offer is in use: declined"
                                                        : newly == 2 ? "the link-local address is in use: trying another"
                                                        : newly == 3 ? "the manual address is in use"
                                                                     : "another host claims our address: defending it");
            conflict_until = now + 10000;
            if (manual && s_manual == 3) s_manual = 4;   /* logged once; stays without an address */
        }
        if (!manual && link == 1 && !autoip_on && how == XHW_NET_HOW_NONE && no_offer && now - link_since >= 4000) {
            xhw_logf("[NET] no DHCP lease 4 s after the link came up: AutoIP too");
            netifapi_autoip_start(&s_netif);
            autoip_on = 1;
        }
        if (how == XHW_NET_HOW_NONE) cur = 0;
        if (cur != last_ip) {
            char a[16], m[16];
            last_ip = cur;
            if (cur) {
                fmt_ip(a, sizeof a, cur);
                fmt_ip(m, sizeof m, cur_mask);
                xhw_logf("[NET] %s %s/%s after %u ms", k_how[how], a, m, now);
                s_mask = cur_mask;
                s_bcast = cur | ~cur_mask;
                s_ip = cur;
                if (!s_up_ms) s_up_ms = now | 1;
            } else {
                xhw_logf("[NET] address lost after %u ms", now);
                s_ip = s_bcast = 0;
            }
        }
        s_how = how;
        set_state(link != 1                                            ? XHW_NET_NO_CABLE
                  : now < conflict_until || (manual && s_manual >= 3) ? XHW_NET_CONFLICT
                  : cur                                                ? XHW_NET_UP
                                                                       : XHW_NET_CONFIG);
        if (s_in_dropped) {
            static uint32_t logged;
            if (s_in_dropped - logged >= 100 || (!logged && s_in_dropped)) {
                logged = s_in_dropped;
                xhw_logf("[NET] %u IPv6 or multicast frames dropped so far", (unsigned)logged);
            }
        }
    }
}

int xhw_net_start(void) {
    if (InterlockedCompareExchange(&s_started, 1, 0) == 0) {
        s_t0 = xhw_time_ns();
        s_state = XHW_NET_CONFIG;
        if (!xhw_thread_start(net_worker, NULL, 0, 64 * 1024)) {
            xhw_logf("[NET] no worker thread");
            s_state = XHW_NET_FAILED;
        }
    }
    return s_state;
}

int xhw_net_state(void) { return s_state; }
uint32_t xhw_net_ip(void) { return s_state == XHW_NET_UP ? s_ip : 0; }
uint32_t xhw_net_bcast(void) { return s_state == XHW_NET_UP ? s_bcast : 0; }
int xhw_net_how(void) { return s_how; }
uint32_t xhw_net_up_ms(void) { return s_up_ms; }
uint32_t xhw_net_link_ms(void) { return s_link_ms; }

void xhw_net_pause(int on) {
    if (!s_nic_on || s_shut || !on == !s_paused) return;
    s_paused = on;
    if (on) nvnetdrv_stop_txrx();
    else nvnetdrv_start_txrx();
    xhw_logf("[NET] %s", on ? "paused" : "running again");
}

void xhw_net_shutdown(void) {
    int i;
    if (!s_started || s_shut) return;
    s_shut = 1;
    xhw_log_tee = NULL;
    for (i = 0; i < 20 && !s_ready && s_state != XHW_NET_FAILED; i++) Sleep(25);   /* the worker mid-start */
    if (s_nic_on) {
        nvnetdrv_stop();   /* interrupt off, NIC reset, its rings freed */
        s_nic_on = 0;
        xhw_log_try("[NET] NIC stopped");
    }
}

void xhw_net_ident(uint8_t mac[6], uint8_t serial[12]) {
    ULONG type, len;
    memset(mac, 0, 6);
    memset(serial, 0, 12);
    ExQueryNonVolatileSetting(XC_FACTORY_ETHERNET_ADDR, &type, mac, 6, &len);
    ExQueryNonVolatileSetting(XC_FACTORY_SERIAL_NUMBER, &type, serial, 12, &len);
}

/* ---- env MX_LOG_UDP (test builds): the log as datagrams ----
 * The tee (under the log lock, one writer at a time) queues each line; a
 * thread below the game's sends them once the network is up, one line a
 * datagram (cut at 1200 bytes), from UDP 41050 to the given address and
 * port (tools/xbox/lan_logd.py), at most 200 a second so the governor never
 * has to step in. Lines from before the address wait in the ring (64 KB;
 * when it fills, the newest are dropped). */
#define LOG_PORT 41050
static Ring s_logring;
static HANDLE s_log_ev;
static uint32_t s_log_ip;
static uint16_t s_log_port;

static void log_tee_fn(const char* line, size_t n) {
    if (n > XHW_UDP_MAX_SEND) n = XHW_UDP_MAX_SEND;
    if (ring_push(&s_logring, line, (uint32_t)n, 0, 0)) SetEvent(s_log_ev);
}

static void log_sender(void* arg) {
    static uint8_t buf[XHW_UDP_MAX_SEND];
    int h = -1, n;
    (void)arg;
    while (!s_shut) {
        WaitForSingleObject(s_log_ev, 500);
        if (s_state != XHW_NET_UP) continue;
        if (h < 0 && (h = xhw_udp_open(LOG_PORT)) < 0) continue;
        s_sock[h].quiet = 1;
        while (!s_shut && s_state == XHW_NET_UP && (n = ring_pop(&s_logring, buf, sizeof buf, NULL, NULL)) >= 0) {
            xhw_udp_send(h, s_log_ip, s_log_port, buf, (uint32_t)n);
            Sleep(5);
        }
    }
}

void xhw_net_log_udp(uint32_t ip, uint16_t port) {
    void* buf = malloc(64 * 1024);
    if (!buf || xhw_log_tee) return;
    ring_init(&s_logring, buf, 64 * 1024);
    s_log_ip = ip;
    s_log_port = port;
    s_log_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    xhw_log_tee = log_tee_fn;
    xhw_net_start();
    xhw_thread_start(log_sender, NULL, -1, 32 * 1024);
}

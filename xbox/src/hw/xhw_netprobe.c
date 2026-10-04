/* xhw_netprobe.c - the network probe and the UDP log, test builds only
 * (docs/lan-plan.md phase 0B; docs/testing.md "Network").
 *
 * env MX_NETPROBE=1 (autopad script): at boot the network starts
 * (xhw_net.c) and the probe logs when the link came up and where the
 * address came from ([NETP] link up after N ms, [NETP] dhcp|autoip|manual
 * a.b.c.d after N ms). Then, on UDP 41001, it broadcasts a beacon every
 * second to 255.255.255.255 (the limited broadcast only, D14 rule 8), logs
 * each new source it hears ([NETP] beacon from a.b.c.d:port), answers pings
 * with a pong of the same size, and once a match runs ([SIMH]'s tick count)
 * pings every peer it has heard 60 times a second for 60 s: [NETP] rtt
 * a.b.c.d: sent, back, lost, min, avg, p99, max. A peer silent for 3 s gets
 * nothing more (D14 rule 9) until it beacons again. tools/xbox/lan_probe.py
 * is the same peer on a PC.
 *
 * env MX_NETPROBE_FLOOD=1 as well: from the first peer heard, the probe
 * tries 2000 datagrams a second at it for 10 s; the transmit governor lets
 * 250 a second through ([NET] tx governor dropped N), the proof that no
 * caller can flood the LAN ([NETP] flood: tried N).
 *
 * env MX_LOG_UDP=<ip>:<port>: every log line also goes to that address as
 * a datagram (tools/xbox/lan_logd.py), from the moment the network is up.
 *
 * The wire is text: "MXNP1 BEACON <mac>", "MXNP1 PING <seq> <t>",
 * "MXNP1 PONG <seq> <t>" (t: the sender's clock in microseconds, echoed),
 * "MXNP1 FLOOD <n>" (ignored by the receiver). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#if XHW_TEST_BUILD
#define PROBE_PORT XHW_NET_DISCOVERY_PORT
#define PEERS 8
#define PING_HZ 60
#define PING_SECS 60
#define PING_MAX (PING_HZ * PING_SECS + 64)
#define PEER_SILENT_US 3000000u
#define FLOOD_HZ 2000
#define FLOOD_SECS 10

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint64_t heard;                    /* the last datagram from it */
    uint32_t sent, back, *rtt;         /* rtt: microseconds of each pong in the window */
} Peer;

static Peer s_peer[PEERS];
static int s_npeers;
static uint64_t s_t0;

static uint64_t now_us(void) { return xhw_time_ns() / 1000u; }
static uint32_t ms(void) { return (uint32_t)((now_us() - s_t0) / 1000u); }

static void fmt_ip(char* out, size_t cap, uint32_t ip) {
    snprintf(out, cap, "%u.%u.%u.%u", ip & 255u, ip >> 8 & 255u, ip >> 16 & 255u, ip >> 24 & 255u);
}

static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static void report(Peer* p) {
    char a[16];
    uint64_t sum = 0;
    uint32_t i, n = p->back;
    fmt_ip(a, sizeof a, p->ip);
    if (!n) {
        xhw_logf("[NETP] rtt %s:%u: %u sent, 0 back", a, (unsigned)p->port, (unsigned)p->sent);
        return;
    }
    qsort(p->rtt, n, sizeof p->rtt[0], cmp_u32);
    for (i = 0; i < n; i++) sum += p->rtt[i];
    xhw_logf("[NETP] rtt %s:%u: %u sent, %u back, lost %u, min %u avg %u p99 %u max %u us", a, (unsigned)p->port,
             (unsigned)p->sent, (unsigned)n, (unsigned)(p->sent - n), (unsigned)p->rtt[0], (unsigned)(sum / n),
             (unsigned)p->rtt[(n * 99u) / 100u < n ? (n * 99u) / 100u : n - 1], (unsigned)p->rtt[n - 1]);
}

static int send_text(int h, uint32_t ip, uint16_t port, const char* s) {
    return xhw_udp_send(h, ip, port, s, (uint32_t)strlen(s));
}

static Peer* find_peer(uint32_t ip, uint16_t port) {
    int i;
    for (i = 0; i < s_npeers; i++)
        if (s_peer[i].ip == ip && s_peer[i].port == port) return &s_peer[i];
    for (i = 0; i < s_npeers; i++)   /* through a NAT an answer can come from another port */
        if (s_peer[i].ip == ip) return &s_peer[i];
    return NULL;
}

static void probe_body(void* arg) {
    static char buf[2048];
    char mac_s[20], line[96];
    uint8_t mac[6], serial[12];
    const char* fl = getenv("MX_NETPROBE_FLOOD");
    int h, i, logged_link = 0, logged_wait = 0, pinging = 0, pinged = 0, flood = fl && *fl == '1', flooding = 0;
    uint32_t my_ip, seq = 0, flood_tried = 0, flood_sent = 0;
    uint64_t next_beacon, next_ping = 0, ping_end = 0, flood_next = 0, flood_end = 0;
    (void)arg;
    s_t0 = now_us();
    xhw_net_ident(mac, serial);
    snprintf(mac_s, sizeof mac_s, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    xhw_logf("[NETP] probe on, MAC %s, UDP %d%s", mac_s, PROBE_PORT, flood ? ", flood test" : "");
    xhw_net_start();
    while (xhw_net_state() != XHW_NET_UP) {
        if (!logged_link && xhw_net_link_ms()) {
            xhw_logf("[NETP] link up after %u ms", (unsigned)xhw_net_link_ms());
            logged_link = 1;
        }
        if (xhw_net_state() == XHW_NET_FAILED) {
            xhw_logf("[NETP] the network failed to start");
            return;
        }
        if (!logged_wait && ms() > 60000) {
            xhw_logf("[NETP] no address after 60 s (state %d)", xhw_net_state());
            logged_wait = 1;
        }
        Sleep(20);
    }
    if (!logged_link) xhw_logf("[NETP] link up after %u ms", (unsigned)xhw_net_link_ms());
    my_ip = xhw_net_ip();
    fmt_ip(line, sizeof line, my_ip);
    {
        static const char* const how[] = { "none", "manual", "dhcp", "autoip" };
        int w = xhw_net_how();
        xhw_logf("[NETP] %s %s after %u ms", how[w >= 0 && w <= 3 ? w : 0], line, (unsigned)xhw_net_up_ms());
    }
    h = xhw_udp_open(PROBE_PORT);
    if (h < 0) {
        xhw_logf("[NETP] UDP %d would not open", PROBE_PORT);
        return;
    }
    next_beacon = now_us();
    for (;;) {
        uint64_t t = now_us(), due;
        uint32_t ip;
        uint16_t port;
        int n;
        if (xhw_net_ip() != my_ip) {
            my_ip = xhw_net_ip();
            fmt_ip(line, sizeof line, my_ip);
            xhw_logf("[NETP] address now %s (state %d)", line, xhw_net_state());
        }
        if (t >= next_beacon) {
            char msg[40];
            snprintf(msg, sizeof msg, "MXNP1 BEACON %s", mac_s);
            if (my_ip) send_text(h, 0xFFFFFFFFu, PROBE_PORT, msg);
            next_beacon += 1000000u;
            if (next_beacon < t) next_beacon = t + 1000000u;
        }
        if (flood && !flooding && !flood_end && s_npeers) {
            flooding = 1;
            flood_next = t;
            flood_end = t + FLOOD_SECS * 1000000ull;
            xhw_logf("[NETP] flood: %d datagrams a second at the first peer for %d s", FLOOD_HZ, FLOOD_SECS);
        }
        if (flooding) {
            if (t >= flood_end) {
                flooding = 0;
                xhw_logf("[NETP] flood: tried %u, sent %u", (unsigned)flood_tried, (unsigned)flood_sent);
            } else {
                for (; flood_next <= t; flood_next += 1000000u / FLOOD_HZ) {
                    char msg[32];
                    snprintf(msg, sizeof msg, "MXNP1 FLOOD %u", (unsigned)flood_tried);
                    flood_tried++;
                    flood_sent += (uint32_t)send_text(h, s_peer[0].ip, s_peer[0].port, msg);
                }
            }
        }
        if (!pinged && !pinging && s_npeers && xhw_autopad_match_tick() > 0) {
            pinging = 1;
            next_ping = t;
            ping_end = t + PING_SECS * 1000000ull;
            xhw_logf("[NETP] match running: pinging %d peer(s) %d times a second for %d s", s_npeers, PING_HZ, PING_SECS);
        }
        if (pinging && t >= next_ping) {
            if (t >= ping_end) {
                Sleep(500);   /* the last pongs */
                while ((n = xhw_udp_recv(h, buf, sizeof buf - 1, &ip, &port)) >= 0) {
                    buf[n] = '\0';
                    if (!strncmp(buf, "MXNP1 PING ", 11)) {   /* still answered */
                        buf[7] = 'O';
                        xhw_udp_send(h, ip, port, buf, (uint32_t)n);
                    }
                }
                for (i = 0; i < s_npeers; i++) report(&s_peer[i]);
                pinging = 0;
                pinged = 1;
                continue;
            }
            for (i = 0; i < s_npeers; i++) {
                char msg[64];
                Peer* p = &s_peer[i];
                if (t - p->heard > PEER_SILENT_US) continue;   /* gone quiet: nothing more to it */
                if (!p->rtt) p->rtt = (uint32_t*)calloc(PING_MAX, sizeof(uint32_t));
                snprintf(msg, sizeof msg, "MXNP1 PING %u %llu", (unsigned)seq, (unsigned long long)now_us());
                if (p->rtt && p->sent < PING_MAX && send_text(h, p->ip, p->port, msg)) p->sent++;
            }
            seq++;
            next_ping += 1000000u / PING_HZ;
            if (next_ping < t) next_ping = t;
        }
        due = next_beacon;
        if (pinging && next_ping < due) due = next_ping;
        if (flooding && flood_next < due) due = flood_next;
        t = now_us();
        xhw_udp_wait(h, due > t ? (uint32_t)((due - t + 999u) / 1000u) : 0);
        while ((n = xhw_udp_recv(h, buf, sizeof buf - 1, &ip, &port)) >= 0) {
            char kind[8];
            unsigned long long a = 0, b = 0;
            Peer* p;
            buf[n] = '\0';
            if (strncmp(buf, "MXNP1 ", 6) != 0) continue;
            if (sscanf(buf + 6, "%7s %llu %llu", kind, &a, &b) < 1) continue;
            p = find_peer(ip, port);
            if (p) p->heard = now_us();
            if (!strcmp(kind, "BEACON")) {
                if (!p && s_npeers < PEERS) {
                    char from[16];
                    p = &s_peer[s_npeers++];
                    p->ip = ip;
                    p->port = port;
                    p->heard = now_us();
                    fmt_ip(from, sizeof from, ip);
                    xhw_logf("[NETP] beacon from %s:%u after %u ms (%s)", from, (unsigned)port, ms(), buf + 13);
                }
            } else if (!strcmp(kind, "PING")) {
                buf[7] = 'O';   /* PING -> PONG: the same bytes back, never more (rule 8) */
                xhw_udp_send(h, ip, port, buf, (uint32_t)n);
            } else if (!strcmp(kind, "PONG")) {
                if (p && p->rtt && pinging && p->back < PING_MAX) p->rtt[p->back++] = (uint32_t)(now_us() - b);
            }
        }
    }
}

void xhw_netprobe_boot(void) {
    const char* e = getenv("MX_LOG_UDP");
    if (e) {
        unsigned a, b, c, d, port;
        if (sscanf(e, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5 && a < 256 && b < 256 && c < 256 && d < 256 &&
            port && port < 65536) {
            xhw_logf("[NETP] log stream to %s", e);
            xhw_net_log_udp(a | b << 8 | c << 16 | d << 24, (uint16_t)port);
        } else {
            xhw_logf("[NETP] MX_LOG_UDP=%s: want a.b.c.d:port", e);
        }
    }
    e = getenv("MX_NETPROBE");
    if (e && *e == '1') xhw_thread_start(probe_body, NULL, 1, 64 * 1024);
}
#else
void xhw_netprobe_boot(void) {}
#endif

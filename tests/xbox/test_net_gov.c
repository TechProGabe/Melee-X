/* xhw_net.c's network conduct (docs/lan-plan.md D14 rules 7-10) on the
 * host: tools/xbox/test_net_gov.py pastes the "conduct" part of xhw_net.c
 * in front of this file.
 *
 * 1. The governor against a model (an independent token bucket in exact
 *    integer arithmetic) under random schedules, and its properties: no
 *    one-second window ever holds more than 250 + 32 datagrams or
 *    256 KB + 32 x 1200 bytes; an even 200 a second loses nothing; 32
 *    back to back after a pause pass and the 33rd doesn't; 2000 a second
 *    offered gives 250 a second; 1200-byte datagrams are held to 256 KB a
 *    second.
 * 2. The broadcast window: never three accepted within a second, and a
 *    broadcast is refused only when taking it would make three.
 * 3. The source and destination filters against a model written from the
 *    rules' wording, over random and edge addresses, for a /24, a /16, a
 *    /30 and a link-local address. */
#define IP(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

static uint64_t rnd_state = 88172645463325252ull;
static uint64_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 7;
    rnd_state ^= rnd_state << 17;
    return rnd_state;
}

/* ---- 1. the governor ---- */
typedef struct {
    uint64_t t;
    uint32_t n;
} Sent;

static void window_check(const Sent* s, int n) {
    int i, j = 0;
    uint64_t bytes = 0;
    for (i = 0; i < n; i++) {   /* windows (t - 1 s, t] ending at each send */
        bytes += s[i].n;
        while (s[j].t + 1000000u <= s[i].t) bytes -= s[j++].n;
        assert(i - j + 1 <= (int)(GOV_RATE + GOV_BURST));
        assert(bytes <= (uint64_t)GOV_BYTES + (uint64_t)GOV_BURST * NET_MAX_SEND);
    }
}

static void gov_test(void) {
    static Sent sent[400000];
    Gov g;
    int run, i, ns;
    uint64_t t;
    /* model: tokens as exact integers (millionths), refilled per microsecond */
    for (run = 0; run < 200; run++) {
        uint64_t mp = GOV_BURST * GOV_ONE, mb = (uint64_t)GOV_BURST * NET_MAX_SEND * GOV_ONE, last;
        uint32_t gap_max = 1 + (uint32_t)(rnd() % 20000);
        t = 1000000u + rnd() % 1000000u;
        last = t;
        gov_init(&g, t);
        ns = 0;
        for (i = 0; i < 20000; i++) {
            uint32_t n = rnd() % 4 ? 1 + (uint32_t)(rnd() % 300) : NET_MAX_SEND;
            uint64_t dt;
            int ok, mok;
            t += rnd() % gap_max;
            dt = t - last;
            last = t;
            if (dt > 10000000u) dt = 10000000u;
            mp = mp + dt * GOV_RATE > GOV_BURST * GOV_ONE ? GOV_BURST * GOV_ONE : mp + dt * GOV_RATE;
            mb = mb + dt * GOV_BYTES > (uint64_t)GOV_BURST * NET_MAX_SEND * GOV_ONE ? (uint64_t)GOV_BURST * NET_MAX_SEND * GOV_ONE
                                                                                    : mb + dt * GOV_BYTES;
            mok = mp >= GOV_ONE && mb >= (uint64_t)n * GOV_ONE;
            if (mok) {
                mp -= GOV_ONE;
                mb -= (uint64_t)n * GOV_ONE;
            }
            ok = gov_take(&g, t, n);
            assert(ok == mok);
            if (ok) {
                sent[ns].t = t;
                sent[ns++].n = n;
            }
        }
        window_check(sent, ns);
    }
    /* an even 200 a second of 600-byte datagrams (the game's input packet
     * rate is ~60): nothing lost */
    gov_init(&g, 0);
    for (i = 0, t = 0; i < 20000; i++, t += 5000) assert(gov_take(&g, t, 600));
    /* a burst after a pause: 32 pass, the 33rd doesn't */
    t += 5000000u;
    for (i = 0; i < 32; i++) assert(gov_take(&g, t, 100));
    assert(!gov_take(&g, t, 100));
    /* 2000 a second offered for 60 s: 250 a second (+ the first burst) */
    gov_init(&g, 0);
    ns = 0;
    for (i = 0, t = 0; i < 120000; i++, t += 500)
        if (gov_take(&g, t, 40)) {
            sent[ns].t = t;
            sent[ns++].n = 40;
        }
    window_check(sent, ns);
    assert(ns >= 250 * 60 && ns <= 250 * 60 + (int)GOV_BURST + 1);
    /* full-size datagrams at 250 a second: the byte bucket holds them to 256 KB a second */
    gov_init(&g, 0);
    ns = 0;
    for (i = 0, t = 0; i < 15000; i++, t += 4000)
        if (gov_take(&g, t, NET_MAX_SEND)) {
            sent[ns].t = t;
            sent[ns++].n = NET_MAX_SEND;
        }
    window_check(sent, ns);
    assert((uint64_t)ns * NET_MAX_SEND <= (uint64_t)GOV_BYTES * 60 + (uint64_t)GOV_BURST * NET_MAX_SEND);
    assert((uint64_t)ns * NET_MAX_SEND >= (uint64_t)GOV_BYTES * 60 - 2 * NET_MAX_SEND);
    printf("governor ok (%d full-size datagrams in 60 s)\n", ns);
}

/* ---- 2. the broadcast window ---- */
static void bcast_test(void) {
    static uint64_t acc[200000];
    int run;
    for (run = 0; run < 100; run++) {
        Gov g;
        int i, na = 0;
        uint64_t t = 5000000u;
        uint32_t gap = 1 + (uint32_t)(rnd() % 1500000u);
        gov_init(&g, 0);
        for (i = 0; i < 2000; i++) {
            int ok, would_ok;
            t += rnd() % gap;
            /* model: refused only if two accepted ones lie within (t - 1 s, t] */
            would_ok = na < 2 || t - acc[na - 2] > 1000000u;
            ok = gov_bcast(&g, t);
            assert(ok == would_ok);
            if (ok) acc[na++] = t;
        }
        for (i = 2; i < na; i++) assert(acc[i] - acc[i - 2] > 1000000u);
    }
    printf("broadcast window ok\n");
}

/* ---- 3. the filters ---- */
static int model_link_local(uint32_t ip) { return (ip & 255u) == 169 && (ip >> 8 & 255u) == 254; }
static int model_same_link(uint32_t a, uint32_t me, uint32_t mask) {
    if (model_link_local(me)) return model_link_local(a);
    return (a & mask) == (me & mask);
}
static int model_bcast(uint32_t a, uint32_t me, uint32_t mask) {
    uint32_t m = model_link_local(me) ? IP(255, 255, 0, 0) : mask;
    return a == 0xFFFFFFFFu || (m != 0xFFFFFFFFu && (a | m) == 0xFFFFFFFFu);
}
static int model_rx(uint32_t src, uint16_t sport, uint32_t me, uint32_t mask) {
    uint32_t o1 = src & 255u;
    if (src == me) return 0;
    if (o1 == 0) return 0;
    if (model_bcast(src, me, mask)) return 0;
    if (o1 >= 224) return 0;
    if (sport == 0) return 0;
    if (!me || o1 == 127 || !model_same_link(src, me, mask)) return 0;
    return 1;
}
static int model_tx(uint32_t dst, uint16_t dport, uint32_t n, uint32_t me, uint32_t mask) {
    uint32_t o1 = dst & 255u;
    if (n > 1200 || !me || dport == 0) return 0;
    if (dst == 0xFFFFFFFFu) return dport == 41001 && n < 200;
    if (dst == me || o1 == 0 || o1 == 127 || o1 >= 224) return 0;
    if (!model_same_link(dst, me, mask)) return 0;
    if (model_bcast(dst, me, mask)) return 0;
    return 1;
}

static uint32_t pick_addr(uint32_t me, uint32_t mask) {
    switch (rnd() % 10) {
    case 0: return 0xFFFFFFFFu;
    case 1: return 0;
    case 2: return me;
    case 3: return me | ~mask;                          /* the directed broadcast */
    case 4: return IP(224 + rnd() % 32, rnd(), rnd(), rnd());   /* multicast, class E */
    case 5: return IP(127, rnd(), rnd(), rnd());
    case 6: return (me & mask) | ((uint32_t)rnd() & ~mask);     /* on the link (or its edges) */
    case 7: return (me & mask) | ((uint32_t)rnd() & ~mask);
    case 8: return IP(169, 254, rnd(), rnd());
    default: return (uint32_t)rnd();
    }
}

static void filter_test(void) {
    static const struct { uint32_t me, mask; } nets[] = {
        { IP(192, 168, 1, 20), IP(255, 255, 255, 0) },  { IP(10, 0, 2, 15), IP(255, 255, 255, 0) },
        { IP(172, 16, 9, 1), IP(255, 255, 0, 0) },      { IP(192, 168, 7, 5), IP(255, 255, 255, 252) },
        { IP(169, 254, 13, 24), IP(255, 255, 0, 0) },   { 0, 0 },
    };
    int k, i, rx_ok = 0, tx_ok = 0;
    for (k = 0; k < (int)(sizeof nets / sizeof nets[0]); k++)
        for (i = 0; i < 300000; i++) {
            uint32_t me = nets[k].me, mask = nets[k].mask;
            uint32_t a = pick_addr(me, mask), n = rnd() % 3 ? (uint32_t)(rnd() % 300) : (uint32_t)(rnd() % 1400);
            uint16_t port = rnd() % 8 == 0 ? 0 : rnd() % 2 ? 41001 : (uint16_t)rnd();
            int r = conduct_rx(a, port, me, mask) == RX_OK, t = conduct_tx(a, port, n, me, mask) == TX_OK;
            if (r != model_rx(a, port, me, mask) || t != model_tx(a, port, n, me, mask)) {
                printf("mismatch: me %08x mask %08x addr %08x port %u n %u: rx %d/%d tx %d/%d\n", me, mask, a, port, n, r,
                       model_rx(a, port, me, mask), t, model_tx(a, port, n, me, mask));
                assert(0);
            }
            rx_ok += r;
            tx_ok += t;
        }
    /* the plain cases by name */
    assert(conduct_tx(IP(192, 168, 1, 30), 41000, 603, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_OK);
    assert(conduct_tx(IP(192, 168, 1, 255), 41001, 100, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_DEST);
    assert(conduct_tx(0xFFFFFFFFu, 41001, 100, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_OK);
    assert(conduct_tx(0xFFFFFFFFu, 41000, 100, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_BCAST_PORT);
    assert(conduct_tx(0xFFFFFFFFu, 41001, 200, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_BCAST_SIZE);
    assert(conduct_tx(IP(8, 8, 8, 8), 41000, 100, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_OFFLINK);
    assert(conduct_tx(IP(192, 168, 1, 30), 41000, 1201, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == TX_SIZE);
    assert(conduct_tx(IP(192, 168, 1, 30), 41000, 100, 0, 0) == TX_NOADDR);
    assert(conduct_tx(IP(169, 254, 200, 1), 41000, 100, IP(169, 254, 13, 24), IP(255, 255, 255, 0)) == TX_OK);
    assert(conduct_tx(IP(192, 168, 1, 30), 41000, 100, IP(169, 254, 13, 24), IP(255, 255, 0, 0)) == TX_OFFLINK);
    assert(conduct_rx(IP(192, 168, 1, 30), 41001, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == RX_OK);
    assert(conduct_rx(IP(192, 168, 1, 20), 41001, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == RX_SELF);
    assert(conduct_rx(IP(192, 168, 2, 30), 41001, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == RX_OFFLINK);
    assert(conduct_rx(IP(192, 168, 1, 30), 0, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == RX_PORT);
    assert(conduct_rx(IP(239, 1, 1, 1), 41001, IP(192, 168, 1, 20), IP(255, 255, 255, 0)) == RX_MCAST);
    assert(rx_ok > 1000 && tx_ok > 1000);
    printf("filters ok (%d received, %d sent of the random cases passed)\n", rx_ok, tx_ok);
}

int main(void) {
    gov_test();
    bcast_test();
    filter_test();
    printf("net conduct ok\n");
    return 0;
}

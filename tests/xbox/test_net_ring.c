/* xhw_net.c's datagram ring (one writer: lwIP's thread; one reader: the
 * game), on the host: tools/xbox/test_net_ring.py pastes the ring in front
 * of this file.
 *
 * 1. Against a model, one thread, random interleaving: every pop returns the
 *    oldest record that was taken and not yet popped, whole (payload, length,
 *    address, port) or cut to the reader's buffer; a push is refused only
 *    when the ring can't hold it (free bytes < its size) or it is longer than
 *    the ring takes, and never refused while twice its size is free; a full
 *    ring drops the new record and counts it (overruns: bursts of pushes with
 *    no pops); nothing is ever written past the ring's end (a guard zone).
 * 2. Two threads, a writer and a reader at full speed: records arrive in
 *    order, intact, and taken + dropped == pushed. */
#include <pthread.h>

#define RSIZE 4096u
#define GUARD 4096u
static uint8_t g_buf[RSIZE + GUARD];   /* the guard zone after the ring must stay untouched */

static void guard_set(void) { memset(g_buf + RSIZE, 0xA5, GUARD); }
static void guard_check(void) {
    uint32_t i;
    for (i = 0; i < GUARD; i++) assert(g_buf[RSIZE + i] == 0xA5);
}

typedef struct {
    uint32_t len, ip;
    uint16_t port;
    uint32_t seed;
} Rec;

static uint8_t pat(uint32_t seed, uint32_t i) { return (uint8_t)((seed * 2654435761u + i * 40503u) >> 13); }

static void fill(uint8_t* p, uint32_t n, uint32_t seed) {
    uint32_t i;
    for (i = 0; i < n; i++) p[i] = pat(seed, i);
}

static void model_test(void) {
    static Rec q[100000];
    static uint8_t tmp[RSIZE], out[RSIZE];
    uint32_t qh = 0, qt = 0, pushes = 0, taken = 0, drops = 0, it, burst = 0;
    Ring r;
    guard_set();
    ring_init(&r, g_buf, RSIZE);
    assert(ring_max(&r) == RSIZE / 2 - 8);
    srand(7);
    for (it = 0; it < 2000000; it++) {
        int push;
        if (burst) {
            push = 1;
            burst--;
        } else if (rand() % 1000 == 0) {
            burst = 40 + rand() % 200;   /* overrun: the reader stalls */
            push = 1;
        } else {
            push = rand() % 2;
        }
        if (push && qh - qt < 100000 - 1) {
            uint32_t n, seed = (uint32_t)rand(), used = r.head - r.tail, need, ok;
            int k = rand() % 16;
            n = k == 0 ? 0 : k == 1 ? ring_max(&r) - (uint32_t)(rand() % 4) : k == 2 ? ring_max(&r) + 1 + (uint32_t)(rand() % 64)
                : k < 8 ? (uint32_t)(rand() % 64) : (uint32_t)(rand() % 700);
            need = ring_need(n);
            fill(tmp, n, seed);
            ok = (uint32_t)ring_push(&r, tmp, n, seed ^ 0x5a5a5a5au, (uint16_t)seed);
            pushes++;
            if (n > ring_max(&r)) assert(!ok);
            else if (RSIZE - used < need) assert(!ok);
            else if (RSIZE - used >= 2 * need) assert(ok);
            if (ok) {
                q[qh % 100000].len = n;
                q[qh % 100000].seed = seed;
                q[qh % 100000].ip = seed ^ 0x5a5a5a5au;
                q[qh % 100000].port = (uint16_t)seed;
                qh++;
            } else {
                drops++;
            }
            assert(r.dropped == drops);
            if (it % 64 == 0) guard_check();
            assert(r.head - r.tail <= RSIZE);
        } else {
            uint32_t ip = 0, cap = rand() % 8 == 0 ? (uint32_t)(rand() % 32) : RSIZE;
            uint16_t port = 0;
            int n = ring_pop(&r, out, cap, &ip, &port);
            if (qt == qh) {
                assert(n == -1);
                assert(ring_empty(&r));
            } else {
                Rec* e = &q[qt % 100000];
                uint32_t want = e->len < cap ? e->len : cap;
                assert(n == (int)want);
                assert(ip == e->ip && port == e->port);
                fill(tmp, e->len, e->seed);
                assert(memcmp(out, tmp, want) == 0);
                qt++;
                taken++;
            }
        }
    }
    while (qt != qh) {
        uint32_t ip;
        uint16_t port;
        int n = ring_pop(&r, out, RSIZE, &ip, &port);
        assert(n == (int)q[qt % 100000].len);
        qt++;
        taken++;
    }
    assert(ring_pop(&r, out, RSIZE, NULL, NULL) == -1);
    assert(taken + drops == pushes);
    guard_check();
    assert(drops > 1000 && taken > 100000);   /* both paths ran */
    printf("model: %u pushed, %u taken, %u dropped\n", pushes, taken, drops);
}

#define THREAD_N 3000000u
static Ring g_tr;
static volatile int g_done;

static void* writer(void* arg) {
    static uint8_t tmp[RSIZE];
    uint32_t i;
    (void)arg;
    for (i = 0; i < THREAD_N; i++) {
        uint32_t n = 4 + (i * 2654435761u >> 22) % 300;
        fill(tmp, n, i);
        memcpy(tmp, &i, 4);
        ring_push(&g_tr, tmp, n, i, (uint16_t)n);
    }
    g_done = 1;
    return NULL;
}

static void thread_test(void) {
    static uint8_t out[RSIZE], want[RSIZE];
    pthread_t t;
    uint32_t got = 0, last = 0;
    int first = 1;
    guard_set();
    ring_init(&g_tr, g_buf, RSIZE);
    pthread_create(&t, NULL, writer, NULL);
    for (;;) {
        uint32_t ip, seq;
        uint16_t port;
        int done = g_done;
        int n = ring_pop(&g_tr, out, RSIZE, &ip, &port);
        if (n < 0) {
            if (done && ring_empty(&g_tr)) break;
            continue;
        }
        memcpy(&seq, out, 4);
        assert(seq == ip && (uint32_t)n == port && (uint32_t)n == 4 + (seq * 2654435761u >> 22) % 300);
        fill(want, (uint32_t)n, seq);
        memcpy(want, &seq, 4);
        assert(memcmp(out, want, (size_t)n) == 0);
        assert(first || seq > last);
        first = 0;
        last = seq;
        got++;
    }
    pthread_join(t, NULL);
    guard_check();
    assert(got + g_tr.dropped == THREAD_N);
    printf("threads: %u pushed, %u taken, %u dropped\n", THREAD_N, got, (unsigned)g_tr.dropped);
}

int main(void) {
    model_test();
    thread_test();
    printf("net ring ok\n");
    return 0;
}

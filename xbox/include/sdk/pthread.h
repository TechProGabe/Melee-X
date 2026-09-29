/* pthread.h - the subset src/pc/audio.c uses, over the Xbox layer's locks.
 * Only on the SDK/src-pc include path; nxdk has no pthreads. */
#ifndef XSDK_PTHREAD_H
#define XSDK_PTHREAD_H
#include "xhw.h"

typedef struct { xhw_mutex* m; } pthread_mutex_t;
typedef struct { int type; } pthread_mutexattr_t;
typedef struct { volatile int done; } pthread_once_t;

#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_ONCE_INIT { 0 }

static inline int pthread_mutexattr_init(pthread_mutexattr_t* a) { a->type = 0; return 0; }
static inline int pthread_mutexattr_settype(pthread_mutexattr_t* a, int t) { a->type = t; return 0; }
static inline int pthread_mutexattr_destroy(pthread_mutexattr_t* a) { (void)a; return 0; }
/* xhw mutexes are always recursive */
static inline int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    (void)a;
    m->m = xhw_mutex_create();
    return m->m ? 0 : -1;
}
static inline int pthread_mutex_lock(pthread_mutex_t* m) { xhw_mutex_lock(m->m); return 0; }
static inline int pthread_mutex_unlock(pthread_mutex_t* m) { xhw_mutex_unlock(m->m); return 0; }

/* Only ever raced by the game thread and the mixer thread at startup, and
 * the mixer thread starts after AXInit ran the once on the game thread. */
static inline int pthread_once(pthread_once_t* o, void (*fn)(void)) {
    if (!__atomic_load_n(&o->done, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(&o->done, 1, __ATOMIC_RELEASE);
    }
    return 0;
}

#endif

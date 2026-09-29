/* xhw_sys.c - time, threads, locks, memory, paths and logging on nxdk.
 * The interface is xbox/include/xhw.h; see docs/architecture.md. */
#include <hal/debug.h>
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

/* ======================================================================
 * Logging: COM1 (xemu's lpc47m157, debug kits), a log-tail ring for the
 * crash/hang reports, and boot.log on the HDD (retail boards have no COM1).
 * ====================================================================== */
static inline unsigned char port_in(unsigned short p) {
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p));
    return v;
}
static inline void port_out(unsigned short p, unsigned char v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p));
}

/* Probe the 16550 scratch register once: on a retail board what an absent
 * port reads back is up to the board and modchip, and spinning on its LSR
 * would take ~0.1 s per byte (OpenCrossing-Xbox traps.md). */
static int s_com1 = -1;
static int com1_present(void) {
    if (s_com1 < 0) {
        port_out(0x3F8 + 7, 0x5A);
        s_com1 = port_in(0x3F8 + 7) == 0x5A;
        port_out(0x3F8 + 7, 0xA5);
        s_com1 = s_com1 && port_in(0x3F8 + 7) == 0xA5;
    }
    return s_com1;
}

static void com1_write(const char* s, size_t n) {
    size_t i;
    if (!com1_present()) return;
    for (i = 0; i < n; i++) {
        int spin = 100000;
        if (s[i] == '\n') {
            while (!(port_in(0x3F8 + 5) & 0x20) && --spin) {}
            port_out(0x3F8, '\r');
            spin = 100000;
        }
        while (!(port_in(0x3F8 + 5) & 0x20) && --spin) {}
        port_out(0x3F8, (unsigned char)s[i]);
    }
}

#define TAIL_SIZE 4096
static char s_tail[TAIL_SIZE];
static volatile unsigned s_tail_pos;
static CRITICAL_SECTION s_log_cs;
static int s_log_cs_init;
static HANDLE s_bootlog = INVALID_HANDLE_VALUE;
static unsigned s_bootlog_bytes;
#define BOOTLOG_MAX (256 * 1024)

void xhw_flush_handle(HANDLE h) {
    IO_STATUS_BLOCK iosb;
    NtFlushBuffersFile(h, &iosb);
}

void xhw_log_open_file(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sboot.log", xhw_save_dir());
    s_bootlog = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void log_write(const char* s, size_t n) {
    size_t i;
    if (!s_log_cs_init) {
        InitializeCriticalSection(&s_log_cs);
        s_log_cs_init = 1;
    }
    EnterCriticalSection(&s_log_cs);
    for (i = 0; i < n; i++) s_tail[(s_tail_pos + i) % TAIL_SIZE] = s[i];
    s_tail_pos += (unsigned)n;
    com1_write(s, n);
    if (s_bootlog != INVALID_HANDLE_VALUE && s_bootlog_bytes < BOOTLOG_MAX) {
        DWORD w;
        WriteFile(s_bootlog, s, (DWORD)n, &w, NULL);
        xhw_flush_handle(s_bootlog);   /* a hard freeze must still leave the line on disk */
        s_bootlog_bytes += (unsigned)n;
    }
    LeaveCriticalSection(&s_log_cs);
}

size_t xhw_log_tail(char* out, size_t cap) {
    unsigned end = s_tail_pos, len = end < TAIL_SIZE ? end : TAIL_SIZE, i;
    if (cap == 0) return 0;
    if (len > cap - 1) len = (unsigned)cap - 1;
    for (i = 0; i < len; i++) out[i] = s_tail[(end - len + i) % TAIL_SIZE];
    out[len] = '\0';
    return len;
}

void xhw_log(const char* line) {
    size_t n = strlen(line);
    log_write(line, n);
    if (n == 0 || line[n - 1] != '\n') log_write("\n", 1);
}

void xhw_logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    log_write(buf, (size_t)n);
    if (n == 0 || buf[n - 1] != '\n') log_write("\n", 1);
}

void xhw_vlog_raw(const char* fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    log_write(buf, (size_t)n);
}

/* ======================================================================
 * Time
 * ====================================================================== */
uint64_t xhw_ticks(void) { return KeQueryPerformanceCounter(); }
uint64_t xhw_ticks_per_sec(void) { return KeQueryPerformanceFrequency(); }

uint64_t xhw_time_ns(void) {
    static uint64_t freq;
    uint64_t t = KeQueryPerformanceCounter();
    if (!freq) freq = KeQueryPerformanceFrequency();
    return (t / freq) * 1000000000ull + (t % freq) * 1000000000ull / freq;
}

int64_t xhw_wallclock_2000(uint32_t* ns_out) {
    /* KeQuerySystemTime: 100 ns units since 1601-01-01 UTC. The dashboard's
     * time zone bias (minutes, UTC = local + bias) makes it local time, which
     * is what the GameCube RTC holds. */
    LARGE_INTEGER t;
    ULONG type, bias = 0, len;
    int64_t units;
    KeQuerySystemTime(&t);
    units = t.QuadPart - 125911584000000000ll;   /* 1601 -> 2000 */
    if (ExQueryNonVolatileSetting(XC_TIMEZONE_BIAS, &type, &bias, sizeof bias, &len) >= 0)
        units -= (int64_t)(LONG)bias * 60 * 10000000ll;
    if (ns_out) *ns_out = (uint32_t)((units % 10000000ll + 10000000ll) % 10000000ll) * 100u;
    return units >= 0 ? units / 10000000ll : -((-units + 9999999) / 10000000ll);
}

void xhw_sleep_ms(uint32_t ms) { Sleep(ms); }
void xhw_yield(void) { SwitchToThread(); }

/* ======================================================================
 * Threads and locks
 * ====================================================================== */
struct xhw_mutex { CRITICAL_SECTION cs; };

xhw_mutex* xhw_mutex_create(void) {
    xhw_mutex* m = (xhw_mutex*)malloc(sizeof *m);
    if (m) InitializeCriticalSection(&m->cs);
    return m;
}
void xhw_mutex_lock(xhw_mutex* m) { EnterCriticalSection(&m->cs); }
void xhw_mutex_unlock(xhw_mutex* m) { LeaveCriticalSection(&m->cs); }

struct xhw_event { HANDLE h; };

xhw_event* xhw_event_create(void) {
    xhw_event* e = (xhw_event*)malloc(sizeof *e);
    if (e) e->h = CreateEventA(NULL, FALSE, FALSE, NULL);
    return e;
}
void xhw_event_signal(xhw_event* e) { SetEvent(e->h); }
int xhw_event_wait(xhw_event* e, uint32_t timeout_ms) {
    return WaitForSingleObject(e->h, timeout_ms) == WAIT_OBJECT_0;
}

uint32_t xhw_tls_alloc(void) { return (uint32_t)TlsAlloc(); }
void* xhw_tls_get(uint32_t slot) { return TlsGetValue((DWORD)slot); }
void xhw_tls_set(uint32_t slot, void* value) { TlsSetValue((DWORD)slot, value); }

typedef struct { void (*fn)(void*); void* arg; } ThreadStart;

static DWORD WINAPI thread_entry(LPVOID p) {
    ThreadStart s = *(ThreadStart*)p;
    free(p);
    xhw_crash_guard(s.fn, s.arg);
    return 0;
}

int xhw_thread_start(void (*fn)(void*), void* arg, int priority, uint32_t stack_bytes) {
    ThreadStart* s = (ThreadStart*)malloc(sizeof *s);
    HANDLE h;
    if (!s) return 0;
    s->fn = fn;
    s->arg = arg;
    h = CreateThread(NULL, stack_bytes ? stack_bytes : 64 * 1024, thread_entry, s, 0, NULL);
    if (!h) {
        free(s);
        return 0;
    }
    if (priority > 2) priority = 2;
    if (priority < -2) priority = -2;
    SetThreadPriority(h, priority);   /* THREAD_PRIORITY_LOWEST..HIGHEST = -2..2 */
    CloseHandle(h);
    return 1;
}

/* ======================================================================
 * Memory
 * ====================================================================== */
void* xhw_alloc_at(uintptr_t va, uint32_t bytes) {
    PVOID base = (PVOID)va;
    SIZE_T size = bytes;
    NTSTATUS st = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(st) || (uintptr_t)base != va) {
        xhw_logf("[MEM] NtAllocateVirtualMemory(%08x, %u KB) failed: %08x", (unsigned)va, bytes / 1024,
                 (unsigned)st);
        return NULL;
    }
    return base;
}

/* ---- demand-committed regions (MEM1, ARAM) ---- */
#define LAZY_CHUNK (64u * 1024)
#define LAZY_MAX 4
typedef struct {
    uintptr_t base;
    uint32_t size;
    volatile LONG* bits;   /* one bit per chunk */
} Lazy;
static Lazy s_lazy[LAZY_MAX];
static volatile LONG s_lazy_n, s_lazy_chunks;

void* xhw_reserve_lazy(uintptr_t va, uint32_t bytes) {
    PVOID base = (PVOID)va;
    SIZE_T size = bytes;
    NTSTATUS st;
    Lazy* l;
    uint32_t words = (bytes / LAZY_CHUNK + 31) / 32;
    if (s_lazy_n >= LAZY_MAX || (va | bytes) & (LAZY_CHUNK - 1)) return NULL;
    st = NtAllocateVirtualMemory(&base, 0, &size, MEM_RESERVE, PAGE_READWRITE);
    if (!NT_SUCCESS(st) || (uintptr_t)base != va) {
        xhw_logf("[MEM] reserve(%08x, %u KB) failed: %08x", (unsigned)va, bytes / 1024, (unsigned)st);
        return NULL;
    }
    l = &s_lazy[s_lazy_n];
    l->bits = (volatile LONG*)calloc(words, sizeof(LONG));
    if (!l->bits) return NULL;
    l->base = va;
    l->size = bytes;
    InterlockedIncrement(&s_lazy_n);
    return base;
}

static int lazy_commit_chunk(Lazy* l, uint32_t chunk) {
    volatile LONG* w = &l->bits[chunk / 32];
    LONG bit = (LONG)(1u << (chunk % 32));
    PVOID base;
    SIZE_T size = LAZY_CHUNK;
    NTSTATUS st;
    if (*w & bit) return 1;
    base = (PVOID)(l->base + chunk * LAZY_CHUNK);
    /* committing a committed page again is harmless, so racing threads are fine */
    st = NtAllocateVirtualMemory(&base, 0, &size, MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) return 0;
    if (!(__atomic_fetch_or(w, bit, __ATOMIC_SEQ_CST) & bit)) InterlockedIncrement(&s_lazy_chunks);
    return 1;
}

static Lazy* lazy_find(uintptr_t a) {
    LONG i, n = s_lazy_n;
    for (i = 0; i < n; i++)
        if (a - s_lazy[i].base < s_lazy[i].size) return &s_lazy[i];
    return NULL;
}

void xhw_commit(const void* p, uint32_t bytes) {
    uintptr_t a = (uintptr_t)p, end = a + bytes;
    Lazy* l;
    if (!bytes || !(l = lazy_find(a))) return;
    if (end > l->base + l->size) end = l->base + l->size;
    for (a = (a - l->base) / LAZY_CHUNK; a <= (end - 1 - l->base) / LAZY_CHUNK; a++)
        if (!lazy_commit_chunk(l, (uint32_t)a))
            xhw_fatal("Out of memory", "The Xbox ran out of memory for the game's main memory or ARAM.");
}

int xhw_lazy_fault(uintptr_t addr) {
    Lazy* l = lazy_find(addr);
    if (!l || KeGetCurrentIrql() >= DISPATCH_LEVEL) return 0;
    return lazy_commit_chunk(l, (uint32_t)((addr - l->base) / LAZY_CHUNK));
}

uint32_t xhw_lazy_committed_kb(void) { return (uint32_t)s_lazy_chunks * (LAZY_CHUNK / 1024); }

uint32_t xhw_mem_free_kb(void) {
    MM_STATISTICS st;
    memset(&st, 0, sizeof st);
    st.Length = sizeof st;
    return MmQueryStatistics(&st) >= 0 ? (uint32_t)(st.AvailablePages * 4) : 0;
}

void xhw_mem_log(const char* where) {
    MM_STATISTICS st;
    memset(&st, 0, sizeof st);
    st.Length = sizeof st;
    if (MmQueryStatistics(&st) >= 0)
        xhw_logf("[MEM] %-18s free %5u KB of %5u KB (image %u KB, virt %u KB, pool %u KB, MEM1+ARAM %u KB)", where,
                 (unsigned)(st.AvailablePages * 4), (unsigned)(st.TotalPhysicalPages * 4),
                 (unsigned)(st.ImagePagesCommitted * 4), (unsigned)(st.VirtualMemoryBytesCommitted / 1024),
                 (unsigned)(st.PoolPagesCommitted * 4), xhw_lazy_committed_kb());
}

/* ======================================================================
 * Paths
 * ====================================================================== */
const char* xhw_game_dir(void) { return "D:\\"; }
const char* xhw_save_dir(void) { return XHW_UDATA_DIR; }

int xhw_mkdir(const char* path) { return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }

static void fill_entry(const WIN32_FIND_DATAA* fd, xhw_dir_entry* out) {
    snprintf(out->name, sizeof out->name, "%s", fd->cFileName);
    out->is_dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = fd->nFileSizeLow;
}

void* xhw_dir_first(const char* pattern, xhw_dir_entry* out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    fill_entry(&fd, out);
    return (void*)h;
}

int xhw_dir_next(void* handle, xhw_dir_entry* out) {
    WIN32_FIND_DATAA fd;
    if (!FindNextFileA((HANDLE)handle, &fd)) {
        FindClose((HANDLE)handle);
        return 0;
    }
    fill_entry(&fd, out);
    return 1;
}

/* pdclib's FILE starts with the kernel file handle (_PDCLIB_fd_t is void*). */
void xhw_flush(void* stdio_file) {
    FILE* f = (FILE*)stdio_file;
    HANDLE h;
    if (!f) return;
    fflush(f);
    h = *(HANDLE*)f;
    if (h && h != INVALID_HANDLE_VALUE) xhw_flush_handle(h);
}

/* Flush FATX's cached directory entries of a whole volume (after a rename). */
void xhw_flush_volume(char drive) {
    char path[] = "\\??\\X:";
    ANSI_STRING name;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h;
    path[4] = drive;
    RtlInitAnsiString(&name, path);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(NtOpenFile(&h, GENERIC_WRITE | SYNCHRONIZE, &oa, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              FILE_SYNCHRONOUS_IO_NONALERT))) {
        NtFlushBuffersFile(h, &iosb);
        NtClose(h);
    }
}

/* ======================================================================
 * 64-bit integer helpers the game triple (i686-pc-windows-gnu) calls by
 * their libgcc names. Built here, with nxdk's triple, the divisions below
 * become nxdk's __alldiv / __aulldiv.
 * ====================================================================== */
long long __divdi3(long long a, long long b) { return a / b; }
unsigned long long __udivdi3(unsigned long long a, unsigned long long b) { return a / b; }
long long __moddi3(long long a, long long b) { return a % b; }
unsigned long long __umoddi3(unsigned long long a, unsigned long long b) { return a % b; }

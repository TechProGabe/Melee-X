/* xhw_crash.c - CPU exception reporter (from OpenCrossing-Xbox xbox_crash.c).
 *
 * Every Xbox thread runs in kernel mode, so an unhandled page fault in game
 * code is a bugcheck: the console freezes or reboots and nothing reaches the
 * disk. xhw_crash_guard() puts an SEH registration record on the thread's
 * stack (fs:[0], what nxdk's own __try uses), so the kernel's dispatcher calls
 * on_exception() first. It writes the fault, the registers and the stack words
 * that point into the XBE to crash.log and to the screen, then parks the
 * thread. It also commits demand-committed memory on first touch
 * (xhw_reserve_lazy). Symbolize with tools/xbox/sym.py and build-xbox/melee_x.map.
 * Kill switch: -DXHW_CRASH_GUARD=0. */
#include <hal/debug.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#ifndef XHW_CRASH_GUARD
#define XHW_CRASH_GUARD 1
#endif

unsigned int xhw_image_base, xhw_image_end;

typedef struct Reg {
    struct Reg* prev;
    void* handler;
} Reg;

enum { DISP_CONTINUE_EXECUTION = 0, DISP_CONTINUE_SEARCH = 1 };

static volatile LONG s_in_crash;
static char s_rep[6144];
static int s_len;

static void rep(const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(s_rep + s_len, sizeof s_rep - (size_t)s_len, fmt, ap);
    va_end(ap);
    if (n > 0) s_len += n;
    if (s_len > (int)sizeof s_rep - 1) s_len = (int)sizeof s_rep - 1;
}

static const char* code_name(ULONG c) {
    switch (c) {
        case 0xC0000005: return "access violation";
        case 0xC000001D: return "illegal instruction";
        case 0xC0000094: return "integer divide by zero";
        case 0xC0000095: return "integer overflow";
        case 0xC0000096: return "privileged instruction";
        case 0xC00000FD: return "stack overflow";
        case 0x80000003: return "breakpoint";
        default: return "exception";
    }
}

static void build_report(const EXCEPTION_RECORD* er, const CONTEXT* cx) {
    PKTHREAD t = KeGetCurrentThread();
    ULONG* sp = (ULONG*)cx->Esp;
    ULONG* top = (ULONG*)t->StackBase;
    int n = 0, i;

    s_len = 0;
    rep("[CRASH] %s (%08lx) at %08lx, frame %u, thread %p\n", code_name((ULONG)er->ExceptionCode),
        (unsigned long)er->ExceptionCode, (unsigned long)(ULONG)er->ExceptionAddress, xhw_frame_count(), (void*)t);
    if ((ULONG)er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2)
        rep("[CRASH] %s of address %08lx\n", er->ExceptionInformation[0] ? "write" : "read",
            (unsigned long)er->ExceptionInformation[1]);
    rep("[CRASH] eip %08lx esp %08lx ebp %08lx eflags %08lx\n", cx->Eip, cx->Esp, cx->Ebp, cx->EFlags);
    rep("[CRASH] eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx\n", cx->Eax, cx->Ebx, cx->Ecx, cx->Edx,
        cx->Esi, cx->Edi);
    rep("[CRASH] free %u KB, lazily committed %u KB\n", xhw_mem_free_kb(), xhw_lazy_committed_kb());
    rep("[CRASH] stack:");
    if (sp && top && sp < top && top - sp < 0x40000) {
        for (; sp < top && n < 48; sp++) {
            ULONG v = *sp;
            if (v >= xhw_image_base + 0x1000 && v < xhw_image_end) {
                rep(" %08lx", v);
                n++;
            }
        }
    }
    rep("\n[CRASH] raw:");
    sp = (ULONG*)cx->Esp;
    for (i = 0; i < 16 && sp && top && sp + i < top; i++) rep(" %08lx", sp[i]);
    rep("\n[CRASH] end\n");
}

static void write_crash_log(void) {
    static char tail[4096];
    HANDLE h = CreateFileA(XHW_UDATA_DIR "crash.log", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    DWORD w;
    size_t tl;
    if (h == INVALID_HANDLE_VALUE) return;
    tl = xhw_log_tail(tail, sizeof tail);
    WriteFile(h, tail, (DWORD)tl, &w, NULL);
    WriteFile(h, s_rep, (DWORD)s_len, &w, NULL);
    xhw_flush_handle(h);
    CloseHandle(h);
}

static void show_screen(void) {
    char* line = s_rep;
    pb_show_debug_screen();
    debugClearScreen();
    debugPrint("Melee-X crashed. Please report it with\n");
    debugPrint(XHW_UDATA_DIR "crash.log\n\n");
    while (*line) {
        char* nl = strchr(line, '\n');
        char buf[112];
        int len = nl ? (int)(nl - line) : (int)strlen(line);
        if (len > (int)sizeof buf - 1) len = (int)sizeof buf - 1;
        memcpy(buf, line, (size_t)len);
        buf[len] = '\0';
        debugPrint("%s\n", buf);
        if (!nl) break;
        line = nl + 1;
    }
    debugPrint("\nHold the power button to switch off.\n");
}

__attribute__((cdecl)) static int on_exception(EXCEPTION_RECORD* er, void* frame, CONTEXT* cx, void* dc) {
    (void)frame;
    (void)dc;
    if (er->ExceptionFlags & EXCEPTION_UNWIND) return DISP_CONTINUE_SEARCH;
    /* first touch of a demand-committed chunk (MEM1, ARAM): commit and retry */
    if ((ULONG)er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2 &&
        xhw_lazy_fault((uintptr_t)er->ExceptionInformation[1]))
        return DISP_CONTINUE_EXECUTION;
    if (InterlockedExchange((LONG*)&s_in_crash, 1)) {
        if (KeGetCurrentIrql() < DISPATCH_LEVEL)
            for (;;) Sleep(1000);
        for (;;) {}
    }
    build_report(er, cx);
    show_screen();
    if (KeGetCurrentIrql() >= DISPATCH_LEVEL) {
        xhw_com1_raw("[CRASH] at raised IRQL\n", 24);
        xhw_com1_raw(s_rep, (size_t)s_len);
        for (;;) {}   /* no waits or file I/O at raised IRQL */
    }
    xhw_log(s_rep);
    write_crash_log();
    for (;;) Sleep(1000);
    return DISP_CONTINUE_SEARCH;
}

__attribute__((noinline)) void xhw_crash_guard(void (*fn)(void*), void* arg) {
    Reg r;
    if (!XHW_CRASH_GUARD) {
        fn(arg);
        return;
    }
    r.handler = (void*)on_exception;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(r.prev));
    __asm__ volatile("movl %0, %%fs:0" : : "r"(&r) : "memory");
    fn(arg);
    __asm__ volatile("movl %0, %%fs:0" : : "r"(r.prev) : "memory");
}

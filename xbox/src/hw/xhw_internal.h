/* xhw_internal.h - shared between the xbox/src/hw files (nxdk triple). */
#ifndef XHW_INTERNAL_H
#define XHW_INTERNAL_H
#include <stdarg.h>
#include <stddef.h>
#include <windows.h>

/* Saves, settings and logs. Always on the HDD, so a burned-disc boot can
 * still save. "MX" 0001; cxbe has no TitleID flag, so the name is fixed here. */
#define XHW_UDATA_ROOT "E:\\UDATA\\4d580001"
#define XHW_UDATA_DIR XHW_UDATA_ROOT "\\"

void xhw_log_open_file(void);
void xhw_vlog_raw(const char* fmt, va_list ap);
size_t xhw_log_tail(char* out, size_t cap);
void xhw_com1_raw(const char* s, size_t n);
void xhw_log_com1_line(const char* line);
void xhw_mem_log(const char* where);
void xhw_flush_handle(HANDLE h);
void xhw_flush_volume(char drive);
void xhw_log_sync(void);               /* flush pending boot.log lines if the lock is free */
int xhw_log_try(const char* line);     /* log + flush without blocking on the lock for long */
int xhw_log_try_file(const char* text);
void xhw_log_keep(const char* name);    /* boot.log closed and kept as name (a console round's chain) */   /* the same, boot.log and the tail only (not COM1) */

/* xhw_crash.c: run fn under the CPU exception reporter (crash.log + screen). */
void xhw_crash_guard(void (*fn)(void*), void* arg);
extern unsigned int xhw_image_base, xhw_image_end;
unsigned xhw_frame_count(void);

/* xhw_fbdump.c: a framebuffer as [FBDUMP] log lines (tools/xbox/fbdump_to_png.py) */
void xhw_fbdump(const void* fb, int w, int h, int bpp, int pitch);
/* the same frame as E:\UDATA\4d580001\shotNN.bmp: screenshots on the console (BACK) */
void xhw_fbdump_file(const void* fb, int w, int h, int bpp, int pitch);
void xgx_shot_next(void);   /* nv2a.c: xhw_fbdump_file the next presented frame */

/* xhw_autopad.c: scripted input from D:\autopad.txt (-DXHW_AUTOPAD=1 only) */
struct xhw_pad;
void xhw_autopad_load(void);
void xhw_autopad_apply(int port, struct xhw_pad* out);
void xhw_autopad_match_end(void);   /* env MX_NEXT_XBE: the next build of a console round, 12 s on */
/* xhw_main.c: launch another XBE by its F:\ or E:\ path (a console round's next build) */
void xhw_launch_xbe(const char* dos_path) __attribute__((noreturn));
int xhw_image_folder(char* out, int size);   /* xhw_main.c: the launched XBE's folder name */

/* xhw_splash.c: "TechProGabe Presents..." title card */
void xhw_splash_show(void);
void xhw_splash_release(void);   /* the mode is about to change */
extern const unsigned char xhw_font16[256 * 16];   /* unscii-16: 8x16, one byte a row, MSB left */

/* xhw_overlay.c: the settings menu's text over a finished frame (CPU writes) */
struct xgx_overlay;
void xhw_overlay_draw(void* fb, int w, int h, int bpp, int pitch, const struct xgx_overlay* o);

/* xhw_watchdog.c: hang dumper (hang.log + screen) */
void xhw_watchdog_start(void);
void xhw_prof_set_game_thread(void);   /* call on the game thread */
void* xhw_game_thread(void);           /* its PKTHREAD, NULL before xhw_prof_set_game_thread */
/* Where a thread that is not running was interrupted: the EIP of the
 * interrupt frame on its kernel stack (and the stack pointer it had), 0 when
 * it is waiting instead. Call at DISPATCH_LEVEL (xhw_prof.c). */
unsigned long xhw_thread_eip(void* kthread, unsigned long* esp_out);
void xhw_perf_calibrate(void);         /* test builds: [CAL], a loop of known length (xhw_perf.c) */
/* xhw_pmc.c: [CPU] at boot (test builds); performance counters and the
 * rotating ablations of the probe build (-DXHW_PMC=1) */
int xhw_running_in_xemu(void);
void xhw_cpu_probe(void);
void xhw_pmc_charge(int bucket);       /* xhw_perf.c: at each bucket switch */
void xhw_pmc_period(void);             /* xhw_perf.c: after each [PERF] line, on the game thread */
void xhw_prof_start(void);             /* -DXHW_PROF=1: sampling profiler (xhw_prof.c) */
void xhw_watchdog_disable(void);
void xhw_watchdog_busy(int on);   /* a long, deliberate stall (screenshot) */

/* xhw_video.c */
void xhw_error_screen(const char* title, const char* const* lines);

/* xhw_audio.c / xhw_pad.c: stop DMA and USB before leaving the XBE (a quick
 * reboot into the next XBE doesn't reset them; OpenCrossing traps.md). */
void xhw_audio_shutdown(void);
void xhw_pad_shutdown(void);

/* xhw_led.c: the front LED back to the SMC. shutdown waits for the write
 * (leaving the XBE); release doesn't (crash: for good; hang report: until
 * the next event). */
void xhw_led_shutdown(void);
void xhw_led_release(int for_good);

/* sdk side (game triple): settings before the video mode is chosen, then
 * the game on the disc image (never returns) */
void xsdk_early(void);
void xsdk_boot(const char* disc_path);

#endif

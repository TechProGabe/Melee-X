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

/* xhw_crash.c: run fn under the CPU exception reporter (crash.log + screen). */
void xhw_crash_guard(void (*fn)(void*), void* arg);
extern unsigned int xhw_image_base, xhw_image_end;
unsigned xhw_frame_count(void);

/* xhw_fbdump.c: a framebuffer as [FBDUMP] log lines (tools/xbox/fbdump_to_png.py) */
void xhw_fbdump(const void* fb, int w, int h, int bpp, int pitch);

/* xhw_autopad.c: scripted input from D:\autopad.txt (-DXHW_AUTOPAD=1 only) */
struct xhw_pad;
void xhw_autopad_load(void);
void xhw_autopad_apply(int port, struct xhw_pad* out);

/* xhw_splash.c: "TechProGabe Presents..." title card */
void xhw_splash_show(void);
void xhw_splash_release(void);   /* the mode is about to change */

/* xhw_watchdog.c: hang dumper (hang.log + screen) */
void xhw_watchdog_start(void);
void xhw_prof_set_game_thread(void);   /* call on the game thread */
void xhw_prof_start(void);             /* -DXHW_PROF=1: sampling profiler (xhw_prof.c) */
void xhw_watchdog_disable(void);
void xhw_watchdog_busy(int on);   /* a long, deliberate stall (screenshot) */

/* xhw_video.c */
void xhw_error_screen(const char* title, const char* const* lines);

/* xhw_audio.c / xhw_pad.c: stop DMA and USB before leaving the XBE (a quick
 * reboot into the next XBE doesn't reset them; OpenCrossing traps.md). */
void xhw_audio_shutdown(void);
void xhw_pad_shutdown(void);

/* sdk side (game triple): settings before the video mode is chosen, then
 * the game on the disc image (never returns) */
void xsdk_early(void);
void xsdk_boot(const char* disc_path);

#endif

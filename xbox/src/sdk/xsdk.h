/* xsdk.h - shared between the Dolphin SDK files in xbox/src/sdk (game triple). */
#ifndef XSDK_H
#define XSDK_H
#include <dolphin/card.h>
#include <stdarg.h>
#include <stddef.h>

/* os.c */
int xsdk_is_game_thread(void);
void xsdk_run_alarms(void);
void xsdk_fill_disc_id(const void* header32);
void xsdk_card_dispatch(CARDCallback callback, s32 chan, s32 result);
void xsdk_log_raw(const char* text);   /* log.c: no newline added */
int xsdk_vsnprintf(char* out, size_t cap, const char* fmt, va_list ap);   /* log.c: vsnprintf with %f/%e/%g */
int xsdk_snprintf(char* out, size_t cap, const char* fmt, ...);

/* dvd.c */
int xsdk_dvd_open(const char* path, char* why, size_t why_cap);
void xsdk_dvd_deliver(void);
void xsdk_dvd_free_dol(void);
/* On the DVD worker, inside a read's completion callback: 1 if [p, p + len)
 * holds bytes that read just brought from the disc image, and where from. */
int xsdk_dvd_disc_source(const void* p, u32 len, u32* image_off);
int xsdk_dvd_image_read(u32 image_off, void* dst, u32 len);   /* dst committed; 1 on success */

/* ar.c */
void xsdk_arq_deliver(void);
void* xsdk_aram_base(void);
u32 xsdk_aram_size(void);
u32 xsdk_aram_disc_kb(void);   /* ARAM contents left on the disc image */

/* pad.c: a controller as PADRead last read it (NULL: not connected) */
struct xhw_pad;
const struct xhw_pad* xsdk_pad_raw(int port);

/* vi.c: frame boundary */
void xsdk_frame_boundary(void);

/* gx backend: end the frame, flip (black: VISetBlack) */
void xsdk_gx_present(int black);
unsigned xsdk_frame_count(void);

#endif

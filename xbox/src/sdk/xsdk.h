/* xsdk.h - shared between the Dolphin SDK files in xbox/src/sdk (game triple). */
#ifndef XSDK_H
#define XSDK_H
#include <dolphin/card.h>
#include <stddef.h>

/* os.c */
int xsdk_is_game_thread(void);
void xsdk_run_alarms(void);
void xsdk_fill_disc_id(const void* header32);
void xsdk_card_dispatch(CARDCallback callback, s32 chan, s32 result);
void xsdk_log_raw(const char* text);   /* log.c: no newline added */

/* dvd.c */
int xsdk_dvd_open(const char* path, char* why, size_t why_cap);
void xsdk_dvd_deliver(void);
void xsdk_dvd_free_dol(void);

/* ar.c */
void xsdk_arq_deliver(void);
void* xsdk_aram_base(void);
u32 xsdk_aram_size(void);

/* vi.c: frame boundary */
void xsdk_frame_boundary(void);

/* gx backend: end the frame, flip (black: VISetBlack) */
void xsdk_gx_present(int black);
unsigned xsdk_frame_count(void);

#endif

/* features.c - melee-pc feature switches and the few aurora extensions the
 * game calls that belong to no SDK library. */
#include <dolphin/thp.h>
#include <stdbool.h>
#include <string.h>

#include "pc/pc.h"

/* Vanilla gameplay: no UCF, no free camera, no unlock-all. */
bool pc_is_ucf_enabled(void) { return false; }
bool pc_is_free_camera_enabled(void) { return false; }
bool pc_is_frozen_stadium_enabled(void) { return false; }
bool pc_is_unlock_all_enabled(void) { return false; }

/* 1 = HUD anchored to the screen edges in 16:9 (widescreen.c). */
int pc_get_hud_mode(void) { return 1; }

/* gobj.c records which GX link is drawing (a debugging aid on aurora). */
unsigned int aurora_draw_tag;

/* THP movies (the intro, character and classic-mode clips): the frames
 * are decoded by thp.c. */
BOOL THPInit(void) { return TRUE; }

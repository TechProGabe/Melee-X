/* xsdk_settings.h - settings.ini in the save folder (settings.c), and the
 * settings menu on the title screen (menu.c). */
#ifndef XSDK_SETTINGS_H
#define XSDK_SETTINGS_H
#include <stdint.h>

#define XSDK_BIND_COUNT 16

typedef struct {
    uint32_t xbox;   /* XHW_BTN_* (one bit) */
    uint16_t gc;     /* PAD_BUTTON_* / PAD_TRIGGER_* (0 = unbound) */
} xsdk_bind;

typedef struct {
    xsdk_bind bind[XSDK_BIND_COUNT];
    float stick_deadzone;    /* radial, fraction of full tilt */
    float cstick_deadzone;
    uint8_t trigger_click;   /* analog value from which L/R also click */
} xsdk_port_settings;

typedef struct {
    int video_720p;          /* 1: use 720p when the dashboard allows it (experimental) */
    int progressive;         /* 1: 480p when the dashboard allows it; 0: 480i */
    int widescreen;          /* 1: 16:9 at 480 when the dashboard says widescreen */
    int fps;                 /* 1: frame-rate counter on screen */
    int ram128;              /* 1: use the RAM above 64 MB on a 128 MB console (untested) */
    float rumble;            /* 0..1 */
    xsdk_port_settings port[4];
} xsdk_settings;

extern xsdk_settings g_xsdk_settings;
extern xsdk_settings g_xsdk_settings_boot;   /* as loaded at boot: what the video mode and memory use */

void xsdk_settings_load(void);
int xsdk_settings_save(void);   /* 0: failed, settings.ini left as it was */
int xsdk_settings_equal(const xsdk_settings* a, const xsdk_settings* b);   /* as settings.ini would hold them */

/* menu.c. The title screen calls xsdk_menu_title_frame every frame
 * (gmtitle.c, PORT); 1: the menu is open and the title stands still.
 * PADRead hands the game neutral input while xsdk_menu_block says so. */
int xsdk_menu_title_frame(void);
int xsdk_menu_block(uint32_t raw_buttons);

#endif

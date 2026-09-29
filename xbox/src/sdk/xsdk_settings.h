/* xsdk_settings.h - settings.ini in the save folder (settings.c). */
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
    int video_720p;          /* 1: use 720p when the dashboard allows it */
    int widescreen;          /* 1: 16:9 at 480 when the dashboard says widescreen */
    float rumble;            /* 0..1 */
    xsdk_port_settings port[4];
} xsdk_settings;

extern xsdk_settings g_xsdk_settings;

void xsdk_settings_load(void);
void xsdk_settings_save(void);

#endif

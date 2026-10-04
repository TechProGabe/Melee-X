/* pad.c - GameCube PAD on the four Xbox controller ports.
 *
 * Player N is physical port N (xbox/src/hw/xhw_pad.c). Default layout,
 * GameCube-like by position (docs/architecture.md):
 *
 *   A -> A      X -> B      B -> X      Y -> Y      White/Black -> Z
 *   Start -> Start          D-pad -> D-pad
 *   left stick -> control stick         right stick -> C-stick
 *   L/R triggers -> analog L/R, digital L/R click near the end of travel
 *
 * Sticks: a radial dead zone (worn Duke/S sticks rest well off centre;
 * OpenCrossing-Xbox measured 18-35%), then scaled so full tilt reaches the
 * GameCube's raw rim (~±104). HSD clamps to the game's 80-unit circle on its
 * own (controller.c), like a real controller. Per-port remapping and the
 * dead zone come from settings.ini (settings.c). */
#include <dolphin/pad.h>
#include <math.h>
#include <string.h>

#include "xhw.h"
#include "xsdk.h"
#include "xsdk_settings.h"

#define PORTS 4
#define STICK_RIM 104.0f

static int s_motor[PORTS];

BOOL PADInit(void) {
    xhw_pad_poll();
    return TRUE;
}

BOOL PADReset(u32 mask) { (void)mask; return TRUE; }
BOOL PADRecalibrate(u32 mask) { (void)mask; return TRUE; }
void PADSetSpec(u32 spec) { (void)spec; }
void PADSetAnalogMode(u32 mode) { (void)mode; }

static void stick(int16_t x, int16_t y, float deadzone, s8* ox, s8* oy) {
    float fx = x / 32767.0f, fy = y / 32767.0f;
    float mag = sqrtf(fx * fx + fy * fy), scale;
    if (mag <= deadzone || mag < 1e-6f) {
        *ox = *oy = 0;
        return;
    }
    if (mag > 1.0f) {
        fx /= mag;
        fy /= mag;
        mag = 1.0f;
    }
    /* rescale [deadzone, 1] -> [0, 1] along the same direction */
    scale = (mag - deadzone) / (1.0f - deadzone) / mag * STICK_RIM;
    fx *= scale;
    fy *= scale;
    *ox = (s8)(fx > 127 ? 127 : fx < -127 ? -127 : lroundf(fx));
    *oy = (s8)(fy > 127 ? 127 : fy < -127 ? -127 : lroundf(fy));
}

static u16 map_buttons(uint32_t b, const xsdk_port_settings* ps) {
    u16 out = 0;
    int i;
    for (i = 0; i < XSDK_BIND_COUNT; i++)
        if (b & ps->bind[i].xbox) out |= ps->bind[i].gc;
    return out;
}

/* the controllers as PADRead last read them, for the settings menu (menu.c) */
static xhw_pad s_raw[PORTS];
static int s_raw_ok[PORTS];

const xhw_pad* xsdk_pad_raw(int port) {
    return port >= 0 && port < PORTS && s_raw_ok[port] ? &s_raw[port] : NULL;
}

u32 PADRead(PADStatus* status) {
    u32 motors = 0;
    uint32_t all = 0;
    int i, block;
    xhw_pad_poll();
    for (i = 0; i < PORTS; i++) {
        s_raw_ok[i] = xhw_pad_get(i, &s_raw[i]);
        if (s_raw_ok[i]) all |= s_raw[i].buttons;
    }
    /* the settings menu is up, or its closing press is still held */
    block = xsdk_menu_block(all);
    for (i = 0; i < PORTS; i++) {
        PADStatus* s = &status[i];
        const xsdk_port_settings* ps = &g_xsdk_settings.port[i];
        xhw_pad p = s_raw[i];
        memset(s, 0, sizeof *s);
        if (!s_raw_ok[i]) {
            s->err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        s->err = PAD_ERR_NONE;
        /* in-game reset: L + R + BACK + BLACK on any controller, back to
         * the dashboard (not the usual BACK + START: L + R + START is
         * Melee's own reset from the pause menu); also with the settings
         * menu up */
        if ((p.buttons & (XHW_BTN_BACK | XHW_BTN_BLACK)) == (XHW_BTN_BACK | XHW_BTN_BLACK) && p.lt > 200 &&
            p.rt > 200) {
            xhw_logf("[PAD] port %d: in-game reset, back to the dashboard", i + 1);
            xhw_exit_reason("in-game reset (L+R+BACK+BLACK)");
            xhw_quit_to_dashboard();
        }
        if (block) {   /* connected, nothing pressed */
            motors |= 0x80000000u >> i;
            continue;
        }
        s->button = map_buttons(p.buttons, ps);
        stick(p.lx, p.ly, ps->stick_deadzone, &s->stickX, &s->stickY);
        stick(p.rx, p.ry, ps->cstick_deadzone, &s->substickX, &s->substickY);
        s->triggerLeft = p.lt;
        s->triggerRight = p.rt;
        if (p.lt >= ps->trigger_click) s->button |= PAD_TRIGGER_L;
        if (p.rt >= ps->trigger_click) s->button |= PAD_TRIGGER_R;
        s->analogA = (s->button & PAD_BUTTON_A) ? 0xFF : 0;
        s->analogB = (s->button & PAD_BUTTON_B) ? 0xFF : 0;
        if (s_motor[i] == PAD_MOTOR_RUMBLE) {
            /* 75% of the motor at rumble = 100: full strength was too much
             * on the console's controllers */
            uint16_t lvl = (uint16_t)(g_xsdk_settings.rumble * 0.75f * 65535.0f);
            xhw_pad_rumble(i, lvl, lvl);
        }
        motors |= 0x80000000u >> i;
    }
    return motors;
}

void PADControlMotor(u32 chan, u32 cmd) {
    static unsigned logged;
    if (chan >= PORTS) return;
    if (cmd == PAD_MOTOR_RUMBLE && !(logged & (1u << chan))) {   /* does the game ask at all? */
        logged |= 1u << chan;
        xhw_logf("[PAD] port %u: rumble on (strength %d%%)", (unsigned)chan + 1,
                 (int)(g_xsdk_settings.rumble * 100.0f + 0.5f));
    }
    if (s_motor[chan] == PAD_MOTOR_RUMBLE && cmd != PAD_MOTOR_RUMBLE) xhw_pad_rumble((int)chan, 0, 0);
    s_motor[chan] = (int)cmd;
}

void PADControlAllMotors(const u32* cmd) {
    int i;
    for (i = 0; i < PORTS; i++) PADControlMotor((u32)i, cmd[i]);
}

/* rumble.c (TARGET_PC) always goes through this; netplay isn't built */
void pc_net_rumble_command(unsigned port, unsigned cmd) { PADControlMotor(port, cmd); }

/* Stick clamps from the SDK, for anything that calls them directly. */
static void clamp_stick(s8* px, s8* py, int max, int min) {
    int x = *px, y = *py, sx = x < 0 ? -1 : 1, sy = y < 0 ? -1 : 1;
    x = x * sx;
    y = y * sy;
    x = x < min ? 0 : x - min;
    y = y < min ? 0 : y - min;
    if (x > max) x = max;
    if (y > max) y = max;
    *px = (s8)(x * sx);
    *py = (s8)(y * sy);
}

void PADClamp(PADStatus* s) {
    int i;
    for (i = 0; i < PORTS; i++) {
        if (s[i].err != PAD_ERR_NONE) continue;
        clamp_stick(&s[i].stickX, &s[i].stickY, 72, 15);
        clamp_stick(&s[i].substickX, &s[i].substickY, 59, 15);
    }
}

void PADClampCircle(PADStatus* s) { PADClamp(s); }

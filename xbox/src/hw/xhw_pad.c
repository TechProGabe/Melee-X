/* xhw_pad.c - the four controller ports through nxdk's SDL2 GameController.
 *
 * Player N is the controller in physical port N, always: nxdk's joystick
 * driver reports the port as the player index (1..4, SDL_xboxjoystick.c
 * xid_get_device_port), independent of the order controllers were plugged
 * in. A controller whose port can't be determined takes the first free slot.
 * The GameCube mapping is done on the SDK side (xbox/src/sdk/pad.c). */
#include <SDL.h>
#include <usbh_lib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#define PORTS 4

static SDL_GameController* s_pad[PORTS];
static SDL_JoystickID s_id[PORTS];
static int s_init;

static void pad_init(void) {
    if (s_init) return;
    s_init = 1;
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        xhw_logf("[PAD] SDL_Init(GAMECONTROLLER) failed: %s", SDL_GetError());
        return;
    }
    SDL_GameControllerEventState(SDL_IGNORE);
}

static void open_device(int device) {
    SDL_GameController* gc;
    SDL_JoystickID id;
    int port, i;
    if (!SDL_IsGameController(device)) return;
    id = SDL_JoystickGetDeviceInstanceID(device);
    for (i = 0; i < PORTS; i++)
        if (s_pad[i] && s_id[i] == id) return;
    port = SDL_JoystickGetDevicePlayerIndex(device) - 1;
    if (port < 0 || port >= PORTS || s_pad[port]) {
        for (port = 0; port < PORTS && s_pad[port]; port++) {}
        if (port == PORTS) return;
    }
    gc = SDL_GameControllerOpen(device);
    if (!gc) return;
    s_pad[port] = gc;
    s_id[port] = id;
    xhw_logf("[PAD] port %d: %s", port + 1, SDL_GameControllerName(gc));
}

void xhw_pad_poll(void) {
    int i, n;
    pad_init();
    SDL_GameControllerUpdate();
    for (i = 0; i < PORTS; i++) {
        if (s_pad[i] && !SDL_GameControllerGetAttached(s_pad[i])) {
            xhw_logf("[PAD] port %d: removed", i + 1);
            SDL_GameControllerClose(s_pad[i]);
            s_pad[i] = NULL;
        }
    }
    n = SDL_NumJoysticks();
    for (i = 0; i < n; i++) open_device(i);
}

/* BACK (unmapped by default): a screenshot of the next frame to E:;
 * Y pressed while BACK is held: flush the texture and display-list caches */
static volatile int s_flush_req;
int xhw_debug_flush_take(void) { return __atomic_exchange_n(&s_flush_req, 0, __ATOMIC_ACQ_REL); }

static void shot_button(int port, uint32_t buttons) {
    static uint32_t s_prev[PORTS];
    uint32_t pressed = buttons & ~s_prev[port];
    if (pressed & XHW_BTN_BACK) xgx_shot_next();
    if ((pressed & XHW_BTN_Y) && (buttons & XHW_BTN_BACK)) __atomic_store_n(&s_flush_req, 1, __ATOMIC_RELEASE);
    s_prev[port] = buttons;
}

int xhw_pad_get(int port, xhw_pad* out) {
    SDL_GameController* gc;
    static const struct { SDL_GameControllerButton b; uint32_t bit; } k_map[] = {
        { SDL_CONTROLLER_BUTTON_A, XHW_BTN_A }, { SDL_CONTROLLER_BUTTON_B, XHW_BTN_B },
        { SDL_CONTROLLER_BUTTON_X, XHW_BTN_X }, { SDL_CONTROLLER_BUTTON_Y, XHW_BTN_Y },
        /* nxdk maps the Duke/S black and white buttons to the shoulders */
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XHW_BTN_BLACK },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XHW_BTN_WHITE },
        { SDL_CONTROLLER_BUTTON_START, XHW_BTN_START }, { SDL_CONTROLLER_BUTTON_BACK, XHW_BTN_BACK },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, XHW_BTN_LSTICK }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, XHW_BTN_RSTICK },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, XHW_BTN_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, XHW_BTN_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, XHW_BTN_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XHW_BTN_RIGHT },
    };
    size_t i;
    memset(out, 0, sizeof *out);
    if (port < 0 || port >= PORTS) return 0;
    if (!(gc = s_pad[port])) {
        xhw_autopad_apply(port, out);
        shot_button(port, out->buttons);
        return out->connected;
    }
    out->connected = 1;
    for (i = 0; i < sizeof k_map / sizeof k_map[0]; i++)
        if (SDL_GameControllerGetButton(gc, k_map[i].b)) out->buttons |= k_map[i].bit;
    out->lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    out->ly = (int16_t)~SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);   /* SDL: y down */
    out->rx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX);
    out->ry = (int16_t)~SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY);
    out->lt = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    out->rt = (uint8_t)(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
    xhw_autopad_apply(port, out);
    shot_button(port, out->buttons);
    return 1;
}

void xhw_pad_rumble(int port, uint16_t low, uint16_t high) {
    if (port < 0 || port >= PORTS || !s_pad[port]) return;
    /* renewed every PADRead while the game holds the motor on */
    if (SDL_GameControllerRumble(s_pad[port], low, high, 100) != 0) {
        static unsigned logged;
        if (!(logged & (1u << port))) {
            logged |= 1u << port;
            xhw_logf("[PAD] port %d: rumble failed (%s)", port + 1, SDL_GetError());
        }
    }
}

void xhw_pad_shutdown(void) {
    int i;
    for (i = 0; i < PORTS; i++) {
        if (s_pad[i]) {
            SDL_GameControllerRumble(s_pad[i], 0, 0, 0);
            SDL_GameControllerClose(s_pad[i]);
            s_pad[i] = NULL;
        }
    }
    /* SDL's joystick quit leaves the OHCI host controller running, and it
     * keeps DMAing into RAM across XLaunchXBE (OpenCrossing traps.md) */
    if (s_init) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        usbh_core_deinit();
    }
    s_init = 0;
}

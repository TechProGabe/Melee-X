/* xhw_pad.c - the four controller ports through nxdk's SDL2 GameController.
 *
 * Player N is the controller in physical port N, always: nxdk's joystick
 * driver reports the port as the player index (1..4, SDL_xboxjoystick.c
 * xid_get_device_port), independent of the order controllers were plugged
 * in. A controller whose port can't be determined takes the first free slot.
 * The GameCube mapping is done on the SDK side (xbox/src/sdk/pad.c).
 *
 * SDL's event queue: nothing here reads events (the state is polled), but
 * SDL_Init(GAMECONTROLLER) starts the event loop, and every stick, trigger
 * or button change became an SDL_JOY* event queued for good, ~80 bytes of
 * malloc each, up to 65535 of them (5 MB). On the console that was the
 * memory leak of the v39-v52 roadmap: free RAM fell in 64 KB steps (the
 * heap's growth) whenever a controller was handled or its sticks jittered,
 * and not at all in CPU-only burn-ins or in xemu, which has no controller
 * attached. Joystick events are now off (the controller layer reads the
 * joystick's state directly, not through events) and the queue is emptied
 * every poll. -DXHW_PAD_DRAIN=0 restores the old behaviour; test builds
 * take `env MX_PAD_NOISE=<n>` (autopad) to queue n axis events a poll the
 * way SDL's joystick code does, which reproduces the growth in xemu. */
#include <SDL.h>
#include <usbh_lib.h>
#include <stdlib.h>
#include <string.h>

#include "xhw.h"
#include "xhw_internal.h"

#define PORTS 4

static SDL_GameController* s_pad[PORTS];
static SDL_JoystickID s_id[PORTS];
static int s_init;

#ifndef XHW_PAD_DRAIN
#define XHW_PAD_DRAIN 1
#endif

static void pad_init(void) {
    if (s_init) return;
    s_init = 1;
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) < 0) {
        xhw_logf("[PAD] SDL_Init(GAMECONTROLLER) failed: %s", SDL_GetError());
        return;
    }
    SDL_GameControllerEventState(SDL_IGNORE);
    if (XHW_PAD_DRAIN) SDL_JoystickEventState(SDL_IGNORE);
}

/* events SDL holds in its queue (the [MEMB] line) */
uint32_t xhw_pad_events_queued(void) {
    int n;
    if (!s_init) return 0;
    n = SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
    return n > 0 ? (uint32_t)n : 0;
}

/* test builds, env MX_PAD_NOISE=<n>: n axis changes a poll, queued as
 * SDL_PrivateJoystickAxis does (only while the event type is enabled) */
static void pad_noise(void) {
    static int s_n;
    static int16_t s_v;
    int i;
    if (!s_n) {   /* asked again each poll: the splash polls before the script is read */
        const char* e = getenv("MX_PAD_NOISE");
        if (!e || (s_n = atoi(e)) <= 0) {
            s_n = 0;
            return;
        }
        xhw_logf("[PAD] test: %d synthetic axis events a poll", s_n);
    }
    for (i = 0; i < s_n; i++) {
        SDL_Event ev;
        if (SDL_GetEventState(SDL_JOYAXISMOTION) != SDL_ENABLE) return;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_JOYAXISMOTION;
        ev.jaxis.axis = (Uint8)(i & 3);
        ev.jaxis.value = s_v++;
        SDL_PushEvent(&ev);
    }
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
    if (XHW_TEST_BUILD) pad_noise();
    if (XHW_PAD_DRAIN) SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
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

/* BACK (unmapped by default): a screenshot of the next frame to E:, in test
 * builds (XHW_TEST_BUILD, xhw.h) or with settings.ini's `screenshots = 1`.
 * Test builds only: Y pressed while BACK is held flushes the texture and
 * display-list caches. */
static volatile int s_flush_req, s_shots;
void xhw_pad_set_shots(int on) { s_shots = on; }
int xhw_debug_flush_take(void) { return __atomic_exchange_n(&s_flush_req, 0, __ATOMIC_ACQ_REL); }

static void shot_button(int port, uint32_t buttons) {
    static uint32_t s_prev[PORTS];
    uint32_t pressed = buttons & ~s_prev[port];
    if (!XHW_TEST_BUILD && !s_shots) return;
    if (pressed & XHW_BTN_BACK) xgx_shot_next();
    if (XHW_TEST_BUILD && (pressed & XHW_BTN_Y) && (buttons & XHW_BTN_BACK))
        __atomic_store_n(&s_flush_req, 1, __ATOMIC_RELEASE);
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

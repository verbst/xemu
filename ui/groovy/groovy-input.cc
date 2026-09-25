/*
 * Groovy MiSTer output for xemu — controllers attached to the display
 *
 * The MiSTer resolves whatever is physically plugged into it to a standard set
 * of button positions before sending them, so a cabinet's own controls arrive
 * here already normalised. That removes the need to describe the device, not
 * the need to configure it: which Xbox control each position drives, and which
 * way a stick points, are still the user's to choose, and are held in the same
 * stored mapping a locally attached pad uses.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include <cstring>

#include "groovy.h"
#include "groovy-internal.hh"
#include "groovy-diagnostics.h"
#include "groovymister.h"

#include "ui/xemu-input.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include "ui/xui/xemu-hud.h"

extern "C" {
#include "qemu/timer.h"
}

#define GROOVY_MAX_PADS 2

namespace {

struct Pad {
    ControllerState *state;
    bool created;
    uint8_t strong, weak;       /* last values sent */
    uint8_t want_strong, want_weak;
};

} /* namespace */

static Pad g_pads[GROOVY_MAX_PADS];

void groovy_input_ports(int *port1, int *port2)
{
    if (port1) {
        *port1 = g_pads[0].state ? g_pads[0].state->bound : -1;
    }
    if (port2) {
        *port2 = g_pads[1].state ? g_pads[1].state->bound : -1;
    }
}

/*
 * The wire packs the four directions into the low bits and button 1 through 12
 * above them; the mapping stores the position rather than the bit, so that the
 * numbering the settings display is the numbering the user sees on the pad.
 */
static uint32_t position_mask(int position)
{
    if (position >= GROOVY_PAD_B1 && position <= GROOVY_PAD_B12) {
        return GM_JOY_B1 << (position - GROOVY_PAD_B1);
    }

    switch (position) {
    case GROOVY_PAD_DPAD_UP:    return GM_JOY_UP;
    case GROOVY_PAD_DPAD_DOWN:  return GM_JOY_DOWN;
    case GROOVY_PAD_DPAD_LEFT:  return GM_JOY_LEFT;
    case GROOVY_PAD_DPAD_RIGHT: return GM_JOY_RIGHT;
    default:                    return 0;   /* unassigned */
    }
}

static int mask_to_position(uint32_t mask)
{
    for (int i = 0; i < GROOVY_PAD_BUTTON_COUNT; i++) {
        if (mask & position_mask(i)) {
            return i;
        }
    }
    return -1;
}

/*
 * Reassignment in the settings, which cannot be driven by events here: a press
 * on a MiSTer pad arrives as a changed bit in the next packet rather than as
 * anything SDL will deliver, so the menu polls for one instead.
 *
 * Only positions pressed after the wait began are eligible, so a button
 * already held when the menu opened does not assign itself, and the assignment
 * is made on release so the new binding does not inherit the press that chose
 * it. Both are how the gamepad path behaves, arrived at differently.
 */
struct Capture {
    bool waiting;
    bool want_axis;         /* an axis row is waiting, not a button row */
    uint32_t previous;      /* last packet's buttons, kept even when idle */
    uint32_t armed;         /* buttons pressed since the wait began */
    uint32_t axis_armed;    /* axes seen at rest since the wait began */
    int result;             /* position chosen, or -1 */
};

static Capture g_capture[GROOVY_MAX_PADS] = {
    { false, false, 0, 0, 0, -1 },
    { false, false, 0, 0, 0, -1 },
};

/*
 * An axis has to be seen near rest before a deflection counts, so a stick held
 * over or a trigger resting high does not assign itself. The two levels are far
 * enough apart that noise around either cannot reach the other.
 */
#define AXIS_AT_REST  8192
#define AXIS_DEFLECTED 16384

static void capture_note(int index, uint32_t joy, const int16_t *wire)
{
    Capture &cap = g_capture[index];
    uint32_t pressed = joy & ~cap.previous;
    uint32_t released = cap.previous & ~joy;

    cap.previous = joy;

    if (!cap.waiting) {
        return;
    }

    if (cap.want_axis) {
        for (int i = 0; i < GROOVY_PAD_AXIS_COUNT; i++) {
            int magnitude = wire[i] < 0 ? -(wire[i] + 1) : wire[i];
            if (magnitude < AXIS_AT_REST) {
                cap.axis_armed |= 1u << i;
            } else if ((cap.axis_armed & (1u << i)) &&
                       magnitude > AXIS_DEFLECTED) {
                cap.result = i;
                cap.waiting = false;
                return;
            }
        }
        return;
    }

    cap.armed |= pressed;

    uint32_t chosen = released & cap.armed;
    if (chosen) {
        cap.result = mask_to_position(chosen);
        cap.waiting = false;
    }
}

void groovy_input_capture_begin(int pad_index, bool want_axis)
{
    if (pad_index < 0 || pad_index >= GROOVY_MAX_PADS) {
        return;
    }
    Capture &cap = g_capture[pad_index];
    cap.armed = 0;
    cap.axis_armed = 0;
    cap.result = -1;
    cap.want_axis = want_axis;
    cap.waiting = true;
}

void groovy_input_capture_cancel(int pad_index)
{
    if (pad_index >= 0 && pad_index < GROOVY_MAX_PADS) {
        g_capture[pad_index].waiting = false;
    }
}

bool groovy_input_capture_press(int pad_index, int *position)
{
    if (pad_index < 0 || pad_index >= GROOVY_MAX_PADS) {
        return false;
    }
    Capture &cap = g_capture[pad_index];
    if (cap.result < 0) {
        return false;
    }

    *position = cap.result;
    cap.result = -1;
    return true;
}

const char *groovy_input_button_name(int position)
{
    static const char *const dpad[] = { "DPad Up", "DPad Down", "DPad Left",
                                        "DPad Right" };
    static char button[12][12];

    if (position >= GROOVY_PAD_B1 && position <= GROOVY_PAD_B12) {
        int n = position - GROOVY_PAD_B1;
        if (button[n][0] == '\0') {
            snprintf(button[n], sizeof(button[n]), "Button %d", n + 1);
        }
        return button[n];
    }
    if (position >= GROOVY_PAD_DPAD_UP && position <= GROOVY_PAD_DPAD_RIGHT) {
        return dpad[position - GROOVY_PAD_DPAD_UP];
    }
    return "Not Assigned";
}

const char *groovy_input_axis_name(int position)
{
    static const char *const names[GROOVY_PAD_AXIS_COUNT] = {
        "Left Stick X",  "Left Stick Y",  "Right Stick X",
        "Right Stick Y", "Left Trigger",  "Right Trigger",
    };

    if (position >= 0 && position < GROOVY_PAD_AXIS_COUNT) {
        return names[position];
    }
    return "Not Assigned";
}

/*
 * The layout a pad starts with, which is the one the MiSTer's own fronting of
 * the generic positions implies: button 1 through 4 are the face buttons, 5
 * and 6 the shoulders, 7 and 8 select and start, 9 and 10 the triggers, 11 and
 * 12 the stick clicks. Shoulders stand in for the original controller's white
 * and black, following what xemu already does for a modern pad.
 *
 * The stored defaults describe a locally attached pad and use SDL's numbering,
 * so a pad reaching us this way has to have its own written over them.
 */
static void apply_default_mapping(GamepadMappings *map)
{
    auto &c = map->controller_mapping;

    c.a = GROOVY_PAD_B1;
    c.b = GROOVY_PAD_B1 + 1;
    c.x = GROOVY_PAD_B1 + 2;
    c.y = GROOVY_PAD_B1 + 3;
    c.lshoulder = GROOVY_PAD_B1 + 4;    /* white */
    c.rshoulder = GROOVY_PAD_B1 + 5;    /* black */
    c.back = GROOVY_PAD_B1 + 6;
    c.start = GROOVY_PAD_B1 + 7;
    c.lstick_btn = GROOVY_PAD_B1 + 10;
    c.rstick_btn = GROOVY_PAD_B1 + 11;

    c.dpad_up = GROOVY_PAD_DPAD_UP;
    c.dpad_down = GROOVY_PAD_DPAD_DOWN;
    c.dpad_left = GROOVY_PAD_DPAD_LEFT;
    c.dpad_right = GROOVY_PAD_DPAD_RIGHT;

    /* Nothing on the wire corresponds to the guide button. Holding the two
     * menu buttons together already opens xemu's own menu, which gives a user
     * with only a MiSTer pad a way in, and the position is left free for
     * anyone who would rather spend a button on it. */
    c.guide = -1;

    c.axis_left_x = GROOVY_PAD_AXIS_LEFT_X;
    c.axis_left_y = GROOVY_PAD_AXIS_LEFT_Y;
    c.axis_right_x = GROOVY_PAD_AXIS_RIGHT_X;
    c.axis_right_y = GROOVY_PAD_AXIS_RIGHT_Y;
    c.axis_trigger_left = GROOVY_PAD_AXIS_LTRIGGER;
    c.axis_trigger_right = GROOVY_PAD_AXIS_RTRIGGER;

    c.invert_axis_left_x = false;
    c.invert_axis_left_y = false;
    c.invert_axis_right_x = false;
    c.invert_axis_right_y = false;
}

void groovy_input_reset_mapping(int pad_index)
{
    if (pad_index < 0 || pad_index >= GROOVY_MAX_PADS ||
        !g_pads[pad_index].state) {
        return;
    }

    char id[35] = { 0 };
    xemu_input_get_controller_identity(g_pads[pad_index].state, id, sizeof(id));
    xemu_settings_reset_controller_mapping(id);
    apply_default_mapping(g_pads[pad_index].state->controller_map);
}

static uint16_t map_buttons(const GamepadMappings *map, uint32_t joy)
{
    const auto &c = map->controller_mapping;
    const struct {
        int position;
        uint16_t button;
    } binds[] = {
        { c.a,          CONTROLLER_BUTTON_A },
        { c.b,          CONTROLLER_BUTTON_B },
        { c.x,          CONTROLLER_BUTTON_X },
        { c.y,          CONTROLLER_BUTTON_Y },
        { c.back,       CONTROLLER_BUTTON_BACK },
        { c.guide,      CONTROLLER_BUTTON_GUIDE },
        { c.start,      CONTROLLER_BUTTON_START },
        { c.lstick_btn, CONTROLLER_BUTTON_LSTICK },
        { c.rstick_btn, CONTROLLER_BUTTON_RSTICK },
        { c.lshoulder,  CONTROLLER_BUTTON_WHITE },
        { c.rshoulder,  CONTROLLER_BUTTON_BLACK },
        { c.dpad_up,    CONTROLLER_BUTTON_DPAD_UP },
        { c.dpad_down,  CONTROLLER_BUTTON_DPAD_DOWN },
        { c.dpad_left,  CONTROLLER_BUTTON_DPAD_LEFT },
        { c.dpad_right, CONTROLLER_BUTTON_DPAD_RIGHT },
    };

    uint16_t b = 0;
    for (const auto &bind : binds) {
        if (joy & position_mask(bind.position)) {
            b |= bind.button;
        }
    }
    return b;
}

static int16_t stick_to_axis(signed char v)
{
    /* The negative end reaches one step further than the positive, so the two
     * halves are scaled separately rather than clipping at full deflection. */
    return (int16_t)(v >= 0 ? (v * 32767) / 127 : (v * 32768) / 128);
}

static int16_t trigger_to_axis(uint8_t v)
{
    return (int16_t)((v * 32767) / 255);
}

/*
 * Reading a stick backwards is a preference, not a property of the hardware,
 * so the flag decides it either way. Y is negated when the flag is clear
 * because down is positive on the wire and up is positive to the guest, which
 * is the same convention a locally attached pad is held to.
 */
static int16_t oriented(int16_t v, bool invert)
{
    return invert ? (int16_t)(-(v + 1)) : v;
}

static int16_t flipped(int16_t v, bool invert)
{
    return invert ? v : (int16_t)(-(v + 1));
}

static void apply_pad(ControllerState *state, uint32_t joy,
                      signed char lx, signed char ly,
                      signed char rx, signed char ry,
                      uint8_t lt, uint8_t rt, bool analog)
{
    const GamepadMappings *map = state->controller_map;
    const auto &c = map->controller_mapping;
    int index = (state == g_pads[0].state) ? 0 : 1;

    state->buttons = map_buttons(map, joy);

    /*
     * Gather the wire's own axes first, then let the mapping decide which of
     * them drives each of the guest's. Building the full set either way keeps
     * the two capability levels from needing separate mapping logic.
     */
    int16_t wire[GROOVY_PAD_AXIS_COUNT] = { 0 };
    if (analog) {
        wire[GROOVY_PAD_AXIS_LEFT_X] = stick_to_axis(lx);
        wire[GROOVY_PAD_AXIS_LEFT_Y] = stick_to_axis(ly);
        wire[GROOVY_PAD_AXIS_RIGHT_X] = stick_to_axis(rx);
        wire[GROOVY_PAD_AXIS_RIGHT_Y] = stick_to_axis(ry);
    }

    /*
     * The triggers also arrive as buttons 9 and 10, which is all a pad without
     * analog triggers or a display that does not forward them ever sends. The
     * analog value wins whenever there is one, so a partial pull stays partial.
     */
    wire[GROOVY_PAD_AXIS_LTRIGGER] = (analog && lt) ? trigger_to_axis(lt)
                                     : (joy & GM_JOY_B9) ? 32767 : 0;
    wire[GROOVY_PAD_AXIS_RTRIGGER] = (analog && rt) ? trigger_to_axis(rt)
                                     : (joy & GM_JOY_B10) ? 32767 : 0;

    capture_note(index, joy, wire);

    auto pick = [&](int position) -> int16_t {
        return (position >= 0 && position < GROOVY_PAD_AXIS_COUNT) ?
                   wire[position] : 0;
    };

    state->axis[CONTROLLER_AXIS_LSTICK_X] =
        oriented(pick(c.axis_left_x), c.invert_axis_left_x);
    state->axis[CONTROLLER_AXIS_RSTICK_X] =
        oriented(pick(c.axis_right_x), c.invert_axis_right_x);
    state->axis[CONTROLLER_AXIS_LSTICK_Y] =
        flipped(pick(c.axis_left_y), c.invert_axis_left_y);
    state->axis[CONTROLLER_AXIS_RSTICK_Y] =
        flipped(pick(c.axis_right_y), c.invert_axis_right_y);

    /* Triggers rest at zero and only travel one way, so inverting them would
     * leave them reading fully pressed. */
    state->axis[CONTROLLER_AXIS_LTRIG] = pick(c.axis_trigger_left);
    state->axis[CONTROLLER_AXIS_RTRIG] = pick(c.axis_trigger_right);

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    if (g_config.groovy.log_level >= 2) {
        const int16_t axes[] = {
            state->axis[CONTROLLER_AXIS_LSTICK_X],
            state->axis[CONTROLLER_AXIS_LSTICK_Y],
            state->axis[CONTROLLER_AXIS_RSTICK_X],
            state->axis[CONTROLLER_AXIS_RSTICK_Y],
            state->axis[CONTROLLER_AXIS_LTRIG],
            state->axis[CONTROLLER_AXIS_RTRIG],
        };
        groovy_diag_mapped(index, state->bound, state->buttons, axes);
    }
}

void groovy_input_note_rumble(int port, uint16_t strong, uint16_t weak)
{
    for (int i = 0; i < GROOVY_MAX_PADS; i++) {
        if (g_pads[i].state && g_pads[i].state->bound == port) {
            g_pads[i].want_strong = (uint8_t)(strong >> 8);
            g_pads[i].want_weak = (uint8_t)(weak >> 8);
            return;
        }
    }
}

void groovy_input_open(void)
{
    if (!g_config.groovy.controls.pads) {
        return;
    }

    /*
     * Binding a controller attaches a USB device to the running machine, which
     * has to happen with the machine held still. The paths that add a local
     * controller are already inside that lock by the time they get here; this
     * one is called from the frame loop and has to take it itself.
     */
    xemu_main_loop_lock();

    for (int i = 0; i < GROOVY_MAX_PADS; i++) {
        if (g_pads[i].created) {
            continue;
        }

        ControllerState *s = (ControllerState *)g_malloc0(sizeof(ControllerState));
        s->type = INPUT_DEVICE_GROOVY_MISTER;
        s->name = i == 0 ? "MiSTer Player 1" : "MiSTer Player 2";
        s->bound = -1;
        s->sdl_joystick_id = i;   /* names the port binding and the mapping */

        g_pads[i].state = s;
        g_pads[i].created = true;

        /* A mapping the user has already adjusted is kept; a freshly created
         * one carries the defaults for a locally attached pad and has to be
         * replaced with the layout that arrives over the wire. */
        if (xemu_input_bindings_reload_map(s)) {
            apply_default_mapping(s->controller_map);
        }

        int port = xemu_input_get_controller_default_bind_port(s, 0);
        if (port < 0) {
            /* Nothing remembered for this pad, so take the first free port
             * rather than displacing a controller the user already has. */
            for (int p = 0; p < 4; p++) {
                if (!xemu_input_get_bound(p)) {
                    port = p;
                    break;
                }
            }
        }
        if (port >= 0 && !xemu_input_get_bound(port)) {
            xemu_input_bind(port, s, 0);
            char buf[128];
            snprintf(buf, sizeof(buf), "Connected '%s' to port %d",
                     s->name, port + 1);
            xemu_queue_notification(buf);
        } else {
            /*
             * Reported rather than passed over, and at the default logging
             * level. The pad still exists and still appears in the settings,
             * but nothing reaches the guest through it, so from the sofa this
             * is indistinguishable from the display having stopped sending --
             * and it survives switching streaming off and on again, because
             * whatever is holding the port is still holding it.
             */
            printf("[groovy] %s could not be given a port; all four are in "
                   "use. Free one and reconnect, or unbind a controller in "
                   "Settings > Input.\n", s->name);
            fflush(stdout);
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "MiSTer: no free port for '%s'; its input is ignored",
                     s->name);
            xemu_queue_notification(buf);
        }

        QTAILQ_INSERT_TAIL(&available_controllers, s, entry);

        if (g_config.groovy.log_level >= 1) {
            printf("[groovy] %s -> port %d\n", s->name,
                   s->bound >= 0 ? s->bound + 1 : -1);
            fflush(stdout);
        }
    }

    xemu_main_loop_unlock();
}

void groovy_input_close(void)
{
    bool any = false;
    for (int i = 0; i < GROOVY_MAX_PADS; i++) {
        any |= g_pads[i].created;
    }
    if (!any) {
        return;
    }

    xemu_main_loop_lock();

    for (int i = 0; i < GROOVY_MAX_PADS; i++) {
        if (!g_pads[i].created) {
            continue;
        }
        if (g_pads[i].state->bound >= 0) {
            xemu_input_bind(g_pads[i].state->bound, NULL, 0);
        }
        QTAILQ_REMOVE(&available_controllers, g_pads[i].state, entry);
        g_free(g_pads[i].state);
        memset(&g_pads[i], 0, sizeof(g_pads[i]));

        g_capture[i] = Capture{ false, false, 0, 0, 0, -1 };
    }

    xemu_main_loop_unlock();
}

void groovy_input_apply(GroovyMister *gm, uint8_t caps)
{
    if (!g_config.groovy.controls.pads) {
        return;
    }

    const fpgaJoyInputs *j = &gm->joyInputs;
    bool analog = (caps & GM_CAP_INPUTS_V2) != 0;

    if (g_pads[0].state && g_pads[0].state->bound >= 0) {
        apply_pad(g_pads[0].state, j->joy1,
                  j->joy1LXAnalog, j->joy1LYAnalog,
                  j->joy1RXAnalog, j->joy1RYAnalog,
                  j->joy1LTAnalog, j->joy1RTAnalog, analog);
    }
    if (g_pads[1].state && g_pads[1].state->bound >= 0) {
        apply_pad(g_pads[1].state, j->joy2,
                  j->joy2LXAnalog, j->joy2LYAnalog,
                  j->joy2RXAnalog, j->joy2RYAnalog,
                  j->joy2LTAnalog, j->joy2RTAnalog, analog);
    }
}

void groovy_input_send_rumble(GroovyMister *gm, uint8_t caps)
{
    if (!g_config.groovy.controls.rumble || !(caps & GM_CAP_RUMBLE)) {
        return;
    }

    for (int i = 0; i < GROOVY_MAX_PADS; i++) {
        Pad &p = g_pads[i];
        if (!p.state || p.state->bound < 0) {
            continue;
        }
        if (!p.state->controller_map->enable_rumble) {
            /* Stop once rather than every frame, so turning it off while a
             * motor is running does not leave it running. */
            p.want_strong = 0;
            p.want_weak = 0;
        }
        /* Both motors travel together, and only when the pair changes: the
         * display repeats the last value until it is replaced, so resending an
         * unchanged pair achieves nothing. */
        if (p.want_strong == p.strong && p.want_weak == p.weak) {
            continue;
        }
        p.strong = p.want_strong;
        p.weak = p.want_weak;
        gm->SendRumble((uint8_t)i, p.strong, p.weak);
    }
}

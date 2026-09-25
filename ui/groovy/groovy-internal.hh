/*
 * Groovy MiSTer output for xemu — types shared between the module's
 * translation units.
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

#ifndef XEMU_GROOVY_INTERNAL_HH
#define XEMU_GROOVY_INTERNAL_HH

#include <cstdint>
#include <cstddef>

/*
 * Everything that can change which modeline we should be using.
 *
 * The settings are part of the key, not just the geometry, because several of
 * them alter the answer for an unchanged guest resolution. Comparing the whole
 * key means a settings change forces a fresh resolve on its own, rather than
 * waiting for the guest to happen to switch modes, which for a running game is
 * never.
 */
struct GroovyModeKey {
    uint16_t width;             /* guest native, before interlace doubling */
    uint16_t height;
    uint8_t src_interlaced;     /* guest is driving an interlaced mode */
    uint8_t widescreen;
    float target_refresh;       /* resolved from config; never derived from
                                 * the pacing interval, which we are about to
                                 * write from the answer */
    uint8_t monitor_preset;
    uint8_t scan_mode;
    uint8_t mode_priority;
    uint8_t keep_res_limit_pct;
    uint8_t aspect;

    bool operator==(const GroovyModeKey &o) const
    {
        return width == o.width && height == o.height &&
               src_interlaced == o.src_interlaced &&
               widescreen == o.widescreen &&
               target_refresh == o.target_refresh &&
               monitor_preset == o.monitor_preset &&
               scan_mode == o.scan_mode &&
               mode_priority == o.mode_priority &&
               keep_res_limit_pct == o.keep_res_limit_pct &&
               aspect == o.aspect;
    }
};

/* A modeline in the form CmdSwitchres wants it. */
struct GroovyMode {
    double pclock_mhz;
    uint16_t hactive, hbegin, hend, htotal;
    uint16_t vactive, vbegin, vend, vtotal;
    uint8_t interlace;      /* 0 progressive, 2 interlaced with a
                             * progressive framebuffer */
    double refresh;         /* field rate; drives the guest's frame pacing */
    bool substituted;       /* the request could not be met as asked */
};

/*
 * Bring up or tear down the modeline engine. Initialisation is deferred until
 * a session actually starts: nothing here should run for a user who never
 * turns the feature on.
 */
bool groovy_modes_init(int monitor_preset, const char *custom_ranges);
void groovy_modes_shutdown(void);

/* Discard cached answers and re-initialise. Needed when the monitor preset
 * changes, because switchres only accepts one during initialisation. */
bool groovy_modes_rebuild(int monitor_preset, const char *custom_ranges);

/* Build the key describing what we should be displaying right now. */
bool groovy_modes_current_key(GroovyModeKey *out);

/*
 * Resolve a key to a modeline. Answers are cached per key: switchres appends
 * to its own mode list on every probe, and probing repeatedly both grows that
 * list without bound and starts returning wrong answers.
 *
 * why receives a short human-readable account of any substitution, suitable
 * for a notification.
 */
bool groovy_modes_resolve(const GroovyModeKey &key, GroovyMode *out,
                          char *why, size_t why_size);

/* Largest frame the streaming client can carry, in pixels at 3 bytes each.
 * The client sizes its transfer from the modeline without clamping it, so an
 * oversized mode reads past the send buffer rather than reporting an error. */
uint32_t groovy_max_pixels(void);

/*
 * Copy the composited frame into dst as tightly packed BGR888 at exactly
 * width x height, scaling as needed. dst must have room for width*height*3
 * bytes; callers size it from a modeline that has already been checked against
 * groovy_max_pixels().
 *
 * flip_required follows the render path's own flag: the accelerated path and
 * the plain framebuffer path disagree about row order.
 *
 * Must run on the thread holding the GL context, with the frame's texture
 * still alive.
 */
bool groovy_capture_to_buffer(unsigned int texture, bool flip_required,
                              uint16_t width, uint16_t height, char *dst);

/*
 * Fill dst with a pattern whose orientation and channel order are unambiguous:
 * a white band along the top edge, then red, green and blue bands going down.
 * A picture that is upside down puts the white band at the bottom, and one
 * with the red and blue channels exchanged shows the bands in the wrong
 * colours. Both are the classic ways a first integration goes wrong, and both
 * are obvious at a glance on the CRT.
 */
void groovy_fill_test_pattern(uint16_t width, uint16_t height, char *dst);

class GroovyMister;

/*
 * Present up to two controllers to the guest, fed from the display rather than
 * from a local device, and take them away again when the session ends.
 */
void groovy_input_open(void);
void groovy_input_close(void);

/* Copy the most recent packet into the controllers the guest can see. */
void groovy_input_apply(GroovyMister *gm, uint8_t caps);

/* Send any motor change accumulated since the last frame. */
void groovy_input_send_rumble(GroovyMister *gm, uint8_t caps);

/* Which guest port each pad reached, or -1 for a pad that got none. */
void groovy_input_ports(int *port1, int *port2);

/* Release the cached capture target. Called when a session ends so a disabled
 * feature holds no GPU memory. */
void groovy_capture_release(void);

#endif

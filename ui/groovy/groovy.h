/*
 * Groovy MiSTer output for xemu
 *
 * Streams rendered frames, audio and input to a MiSTer FPGA acting as a
 * network-attached analog GPU for CRT displays.
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

#ifndef XEMU_GROOVY_H
#define XEMU_GROOVY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exercise the vendored modeline calculator and streaming client without
 * touching a display or a socket, logging what each reports. Runs at startup
 * when XEMU_GROOVY_SELFTEST is set in the environment. Returns true if both
 * libraries responded as expected.
 */
bool groovy_selftest(void);

/*
 * Bring the session into line with the current settings. Cheap to call every
 * frame; it only acts when something has actually changed. Nothing in this
 * module touches a socket or the modeline engine until a session starts.
 */
void groovy_update_session(void);

/* Shut the session down and release everything. Safe if none was started. */
void groovy_shutdown(void);

/* True while a session is live and frames should be captured. */
bool groovy_is_streaming(void);

/*
 * A short description of what is currently being displayed, or NULL if nothing
 * has been resolved yet. Owned by the module; valid until the mode changes.
 */
const char *groovy_status_mode(void);

/*
 * True when this thread may capture. Streaming plus a check that we are on the
 * thread that owns the client: the window system can re-enter the render path
 * from its own event handling during a drag or resize, and the client is not
 * safe to use from there.
 */
bool groovy_capture_enabled(void);

/*
 * Geometry to describe the picture by when the accelerated path has not
 * produced one, so the mode still comes from what is actually being shown
 * rather than from a stale or empty reading.
 */
void groovy_set_fallback_geometry(unsigned int width, unsigned int height);

/*
 * Controls as the MiSTer numbers them.
 *
 * Whatever is physically plugged into the display is resolved to this one
 * layout before it reaches us, so these positions are all the identity a pad
 * has here. They are what a MiSTer pad's stored mapping holds, in place of the
 * SDL button and axis numbers a locally attached pad would use.
 */
enum {
    GROOVY_PAD_B1 = 0,
    GROOVY_PAD_B12 = 11,
    GROOVY_PAD_DPAD_UP = 12,
    GROOVY_PAD_DPAD_DOWN,
    GROOVY_PAD_DPAD_LEFT,
    GROOVY_PAD_DPAD_RIGHT,
    GROOVY_PAD_BUTTON_COUNT,
};

enum {
    GROOVY_PAD_AXIS_LEFT_X = 0,
    GROOVY_PAD_AXIS_LEFT_Y,
    GROOVY_PAD_AXIS_RIGHT_X,
    GROOVY_PAD_AXIS_RIGHT_Y,
    GROOVY_PAD_AXIS_LTRIGGER,
    GROOVY_PAD_AXIS_RTRIGGER,
    GROOVY_PAD_AXIS_COUNT,
};

/*
 * Names for those positions, for the mapping table in the settings. Both
 * return a description of an unassigned control rather than NULL when the
 * position is outside the layout.
 */
const char *groovy_input_button_name(int position);
const char *groovy_input_axis_name(int position);

/*
 * Put a pad's mapping back to the layout it starts with. Covers both halves --
 * clearing what was stored and writing the wire layout over the defaults meant
 * for a locally attached pad -- because only this module knows the second.
 */
void groovy_input_reset_mapping(int pad_index);

/*
 * Wait for the user to choose a control, for reassigning one from the
 * settings. Presses arrive over the network rather than as events, so the menu
 * starts a wait and then polls for the answer.
 *
 * A wait is for a button or for an axis, never both, so that nudging a stick
 * while choosing a button does not settle the question. The position that
 * comes back is numbered in whichever of the two layouts was asked for.
 */
void groovy_input_capture_begin(int pad_index, bool want_axis);
void groovy_input_capture_cancel(int pad_index);
bool groovy_input_capture_press(int pad_index, int *position);

/*
 * Record the motor levels the guest has asked for on a bound port. Called from
 * the input layer as the guest writes them; the values are coalesced and sent
 * once per frame rather than forwarded individually.
 */
void groovy_input_note_rumble(int port, uint16_t strong, uint16_t weak);

/*
 * Capture the finished frame and put it on the wire. Called from the render
 * path with the composited texture, before any time is spent drawing the local
 * window, so that the window's cost overlaps the transfer rather than delaying
 * it.
 */
void groovy_frame_captured(unsigned int texture, bool flip_required);

/*
 * Wait up to half a field while streaming, leaving time for capture and
 * transmission. Negative when not streaming, meaning wait without a deadline.
 */
int64_t groovy_acquire_budget_ns(void);

/*
 * Send the previous frame again, for a frame the renderer did not finish in
 * time. Keeps the display acknowledging, which is what the pacing and the
 * audio clock are taken from.
 */
void groovy_frame_repeat(void);

/*
 * Pace the frame. Blocks until the MiSTer is ready for the next one, and holds
 * the loop to the modeline period even on frames where nothing was sent, so
 * that a path which skips a blit does not also skip the pacing.
 */
void groovy_frame_step(void);

/*
 * Audio hand-off between the emulated audio processor and the frame loop.
 *
 * groovy_audio_push is called from the audio processor's own thread and never
 * blocks: that thread is pacing real-time audio, so any delay is audible.
 */
bool groovy_audio_active(void);
void groovy_audio_set_active(bool active);
void groovy_audio_push(const void *pcm, size_t bytes);
size_t groovy_audio_available(void);
size_t groovy_audio_pop(void *dst, size_t bytes);
void groovy_audio_stats(uint32_t *dropped_bytes, uint32_t *starved_frames);

/*
 * The display's clock, applied to the audio processor.
 *
 * While the display is playing what it is sent, the frame loop advances a
 * budget of bytes the audio processor may have produced, from the display's
 * own frame count. The processor is paced against that instead of against the
 * host's sound device, so the two never drift apart; the lead is how far
 * ahead it starts, which is what the display then holds in reserve. Silence
 * sent in its place is taken off again, so it does not have to make it up.
 *
 * groovy_audio_pacing is called from the audio processor's thread. It answers
 * false when the display is not driving the clock, and the caller falls back
 * to its own pacing. The position is how far ahead of the budget the
 * processor is, negative when behind.
 */
/* Consumer only: discard old PCM and establish a new production budget. */
size_t groovy_audio_reset(uint32_t lead_bytes);
void groovy_audio_budget_add(int32_t bytes);
bool groovy_audio_pacing(int *queued, int *low, int *high);
int32_t groovy_audio_budget_position(void);

/*
 * Gain to apply to the host's own audio output, 0 while the user is listening
 * through the display instead. The stream stays open at zero rather than being
 * closed: whenever the display is not driving the audio clock, the audio
 * processor paces itself against how much audio the host has yet to play, and
 * with no stream it falls back to a free-running clock.
 */
float groovy_local_audio_gain(float configured);

/*
 * Deliver one vblank to the guest immediately, from whichever thread is pacing
 * frames. Defined by the UI so the streaming module does not need to know how
 * the guest's timing is wired.
 */
void xemu_process_vblank_now(void);

/*
 * Whether the local window should still draw the emulated frame. Off means the
 * user is watching the CRT; the window keeps running so its menus stay usable.
 */
bool groovy_mirror_enabled(void);

/*
 * Host vsync must be off while streaming: the MiSTer's raster is the clock,
 * and a second limiter on top of it beats against the first. This reports the
 * setting's effective value without disturbing what the user chose, so their
 * preference returns when streaming stops.
 */
bool xemu_vsync_effective(void);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Groovy MiSTer output for xemu — session lifecycle and frame pacing
 *
 * Owns the streaming client. Everything here runs on the thread that renders
 * the local window: the client is not safe to use from more than one thread,
 * and its automatic frame-delay mode measures the time between its own pacing
 * calls, which only means "time spent producing a frame" when those calls
 * happen on the thread doing the producing.
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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>

#include <SDL3/SDL.h>

#include "groovy.h"
#include "groovy-internal.hh"
#include "groovy-diagnostics.h"
#include "groovymister.h"

#include "ui/xemu-settings.h"
#include "ui/xemu-notifications.h"
#include "ui/xui/xemu-hud.h"

extern "C" {
#include "qemu/timer.h"
}

/* Under the shortest idle timeout the display offers, which is five seconds and
 * cannot be read back, with room to spare for a lost datagram. */
#define KEEPALIVE_INTERVAL_MS 2000

/* The most one audio command may carry: the display buffers a command in
 * 8192 stereo samples of DDR before playing it. */
#define GROOVY_AUDIO_MAX_PER_FRAME 32768

namespace {

struct Session {
    std::unique_ptr<GroovyMister> gm;
    bool connected = false;

    bool have_key = false;
    GroovyModeKey key {};
    GroovyMode mode {};
    bool mode_valid = false;

    /*
     * Strictly increasing for the life of the process, including across
     * reconnects. The core stamps returned input packets with the frame number
     * we last sent, and the client discards anything not newer than the last
     * one it saw, so a counter that ever goes backwards silences the pads until
     * it climbs back past its old value.
     */
    uint32_t frame = 0;

    uint64_t last_send_ms = 0;  /* frames count too */
    int64_t next_frame_ns = 0;
    uint64_t period_ns = 16666666;

    /* What the pacing interval was before we took it over. */
    uint64_t saved_vblank_ns = 0;
    bool vblank_overridden = false;

    uint8_t caps = 0;           /* what the display actually granted */

    /*
     * The parameters that ride in the connect message. They cannot be changed
     * on a live session, so a change to any of them has to tear it down and
     * build a new one.
     */
    int init_codec = -1;
    int init_rgb_mode = -1;
    int init_mtu = -1;
    bool init_audio = false;
    int init_near = -1;
    int init_pack = -1;

    uint32_t epoch = 0;         /* reconnects the client has performed for us */

    bool have_frame = false;    /* the blit buffer holds a captured frame */

    /*
     * The display's audio clock. Each acknowledged frame count is worth a
     * fixed number of bytes at the mode's exact rate; the fraction is carried
     * so nothing is lost to rounding. What the display has played and what it
     * has been sent are kept apart: their difference is its reserve.
     */
    double audio_bytes_per_count = 0.0;
    double audio_carry = 0.0;
    uint32_t audio_last_count = 0;
    int64_t audio_played = 0;
    int64_t audio_sent = 0;
    bool audio_primed = false;

    char status[96] = { 0 };
};

} /* namespace */

static Session g_s;

static inline int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static SDL_ThreadID g_owner_thread;
static bool g_owner_thread_known;

/* xemu drives the guest's frame timing from these. */
extern "C" uint64_t vblank_interval_ns;
extern "C" bool vblank_externally_driven;
extern "C" int64_t vblank_last_external_ns;
extern "C" uint64_t vblank_count_timer;
extern "C" uint64_t vblank_count_external;

/*
 * With one clock, the guest is given its vblank by the same loop that paces
 * frames onto the wire, so it produces exactly one frame per frame the CRT
 * scans. With two, the guest keeps its own timer at the same nominal rate and
 * the small difference between the two shows up as an occasional repeated or
 * dropped frame.
 *
 * The cost of two clocks is larger than that suggests. The loops drift freely
 * against each other, so a frame is sampled at an arbitrary point in the
 * guest's own cycle: on average half a frame after it was drawn, and sometimes
 * nearly a whole one.
 */
static bool single_clock_wanted(void)
{
    return g_config.groovy.timing.pacing ==
           CONFIG_GROOVY_TIMING_PACING_SINGLE_CLOCK;
}

bool groovy_is_streaming(void)
{
    return g_s.gm != nullptr;
}

const char *groovy_status_mode(void)
{
    return g_s.status[0] ? g_s.status : nullptr;
}

bool groovy_mirror_enabled(void)
{
    return !groovy_is_streaming() || g_config.groovy.mirror;
}

bool xemu_vsync_effective(void)
{
    return g_config.display.window.vsync && !groovy_is_streaming();
}

bool groovy_capture_enabled(void)
{
    if (!groovy_is_streaming() || !g_s.connected || !g_s.mode_valid) {
        return false;
    }
    /* Normal and live-resize frames must share the client's owner thread. */
    return g_owner_thread_known && SDL_GetCurrentThreadID() == g_owner_thread;
}

static uint64_t now_ms(void)
{
    return qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

/*
 * How long the streaming client believes a frame lasts.
 *
 * Deliberately not derived from the refresh rate the modeline was resolved at,
 * but computed the same way the client computes it. The two have to agree
 * exactly: the client paces the wire and this paces the guest, and any
 * difference between them is a slow beat that shows up as a repeated frame
 * every few seconds.
 *
 * The client measures a line in tenths of a microsecond, so this rounds to the
 * same grid. That quantisation is why the result is not quite the rate the mode
 * was resolved at -- close enough that nothing is visible, and identical on
 * both sides, which is what matters.
 */
static uint64_t client_frame_period_ns(const GroovyMode &mode)
{
    if (mode.pclock_mhz <= 0.0 || mode.htotal == 0 || mode.vtotal == 0) {
        return 0;
    }
    uint64_t line_tenths = (uint64_t)(10.0 * mode.htotal / mode.pclock_mhz + 0.5);
    uint64_t frame_ns = line_tenths * 100 * mode.vtotal;
    if (mode.interlace) {
        frame_ns /= 2;
    }
    return frame_ns;
}

static void set_pacing_from_mode(void)
{
    if (!g_s.mode_valid || g_s.mode.refresh <= 1.0) {
        return;
    }

    uint64_t period = client_frame_period_ns(g_s.mode);
    g_s.period_ns = period ? period
                           : (uint64_t)(1000000000.0 / g_s.mode.refresh + 0.5);

    if (!g_s.vblank_overridden) {
        g_s.saved_vblank_ns = qatomic_read(&vblank_interval_ns);
        g_s.vblank_overridden = true;
    }
    /*
     * The guest has to produce frames at the rate the CRT will scan them,
     * otherwise the two clocks beat and the core either repeats or drops a
     * frame every few seconds.
     */
    qatomic_set(&vblank_interval_ns, g_s.period_ns);
}

static void restore_pacing(void)
{
    /* Hand the guest's timing back before dropping the override, so there is
     * never a moment with no source of vblank. */
    qatomic_set(&vblank_externally_driven, false);

    if (g_s.vblank_overridden) {
        qatomic_set(&vblank_interval_ns, g_s.saved_vblank_ns);
        g_s.vblank_overridden = false;
    }
}

static void teardown(const char *reason)
{
    if (g_s.gm) {
        if (g_s.connected) {
            /*
             * Send the close synchronously first. The ordinary close posts it
             * through the same asynchronous send path the frames use and then
             * immediately dismantles the queues that send would have completed
             * on, so the message can be discarded before it ever leaves. The
             * display is then left holding the last frame instead of returning
             * to its idle state, which looks from the sofa like the picture
             * simply froze.
             *
             * Sending it twice is harmless: the display treats a second close
             * on an already-closed session as nothing to do.
             */
            g_s.gm->CmdSendClose();
            g_s.gm->CmdClose();
        }
        g_s.gm.reset();
    }
    g_s.status[0] = '\0';
    groovy_audio_set_active(false);
    groovy_input_close();
    g_s.caps = 0;
    g_s.connected = false;
    g_s.mode_valid = false;
    g_s.have_key = false;
    g_s.have_frame = false;
    g_s.audio_primed = false;
    restore_pacing();
    groovy_capture_release();
    groovy_modes_shutdown();
    groovy_diag_finish();

    if (reason && reason[0]) {
        xemu_queue_notification(reason);
    }
}

static bool apply_mode(const GroovyMode &mode, const char *why)
{
    /*
     * Mandatory after every connect, and after every mode change. Without it
     * the core has no timings and silently discards the stream, which looks
     * exactly like a dead connection.
     */
    if (g_s.gm->CmdSwitchres(mode.pclock_mhz, mode.hactive, mode.hbegin,
                             mode.hend, mode.htotal, mode.vactive, mode.vbegin,
                             mode.vend, mode.vtotal, mode.interlace) != 0) {
        return false;
    }

    /*
     * Re-assert the input subscription. The display reads it once when the
     * session is initialised, and a mode change is the point at which input
     * has been seen to stop arriving with the video still healthy. Whether the
     * display actually drops it there is unconfirmed; the subscription is a
     * single byte and asserting it again costs nothing, where being wrong the
     * other way costs a controller until the process is restarted.
     */
    g_s.gm->ResendInputSubscribe();

    g_s.mode = mode;
    g_s.mode_valid = true;
    set_pacing_from_mode();

    /*
     * The display counts fields, and plays 48 kHz stereo 16-bit from the same
     * crystal that times them, so a field is worth exactly this much audio.
     * The exact modeline is used rather than the client's pacing grid; only
     * the display's own arithmetic keeps the two clocks locked.
     */
    double field_s = (double)mode.htotal * mode.vtotal / (mode.pclock_mhz * 1e6);
    if (mode.interlace) {
        field_s /= 2.0;
    }
    g_s.audio_bytes_per_count = 192000.0 * field_s;
    /* A mode change resets the display's audio path, lead included. */
    g_s.audio_primed = false;

    snprintf(g_s.status, sizeof(g_s.status), "%ux%u at %.3f Hz, %s",
             mode.hactive, mode.vactive, mode.refresh,
             mode.interlace ? "interlaced" : "progressive");

    if (g_config.groovy.log_level >= 1) {
        /* The same arithmetic the client uses to decide how long a frame
         * lasts, so a disagreement between the mode we asked for and the pace
         * we actually get is visible rather than inferred. */
        /* Reported on the client's own grid, so a disagreement between the
         * mode we asked for and the pace we get is visible rather than
         * inferred. */
        double frame_ns = (double)client_frame_period_ns(mode);
        double line_ns = mode.vtotal ? frame_ns * (mode.interlace ? 2.0 : 1.0)
                                           / mode.vtotal
                                     : 0.0;
        printf("[groovy] modeline %ux%u  pclock %.6f MHz  htotal %u  vtotal %u"
               "  interlace %u\n"
               "[groovy]   line %.0f ns, frame %.3f ms -> %.3f Hz"
               " (switchres said %.3f Hz)\n",
               mode.hactive, mode.vactive, mode.pclock_mhz, mode.htotal,
               mode.vtotal, mode.interlace, line_ns, frame_ns / 1e6,
               1e9 / frame_ns, mode.refresh);
        fflush(stdout);
    }

    if (why && why[0]) {
        char msg[224];
        snprintf(msg, sizeof(msg), "MiSTer: %s", why);
        xemu_queue_notification(msg);
    }
    return true;
}

/*
 * Detail level, 0 meaning lossless. Stored as a plain number rather than a
 * named set, so a hand-edited settings file can put anything here; the client
 * only understands 0 to 3.
 */
static uint8_t near_level(void)
{
    int level = g_config.groovy.stream.nlc_near_level;
    if (level < 0) {
        level = 0;
    } else if (level > 3) {
        level = 3;
    }
    return (uint8_t)level;
}

static bool connect_session(void)
{
    g_s.gm.reset(new GroovyMister());
    groovy_diag_attach(g_s.gm.get());

    /*
     * Both of these have to precede the connection. The subscription opens the
     * return path the display sends controller state on, and the capability
     * request rides in the connect message itself -- there is no way to ask
     * for either afterwards.
     */
    if (g_config.groovy.controls.pads) {
        g_s.gm->BindInputs(g_config.groovy.host, 32101);
        uint8_t caps = GM_CAP_INPUTS_V2;
        if (g_config.groovy.controls.rumble) {
            caps |= GM_CAP_RUMBLE;
        }
        g_s.gm->setInputCaps(caps);
    }

    /* The display drops a silent client only if asked to, and asking promises
     * the keepalive below. It is what frees the CRT when xemu is killed rather
     * than closed. Not an input capability. */
    g_s.gm->setKeepAlive(1);

    g_s.gm->setAutoReconnect(g_config.groovy.auto_reconnect ? 1 : 0);
    g_s.gm->setNearLevel(near_level());
    g_s.gm->setNlcPack(
        g_config.groovy.stream.nlc_pack == CONFIG_GROOVY_STREAM_NLC_PACK_RICE ? 2 : 1);

    /*
     * The near-lossless codec is the only one that keeps up with 480p 3D
     * content, and it only accepts 24-bit colour. Forcing it here rather than
     * letting the core refuse the session keeps the failure out of the user's
     * way.
     */
    uint8_t rgb_mode = g_config.groovy.stream.rgb_mode;
    if (g_config.groovy.stream.codec == CONFIG_GROOVY_STREAM_CODEC_NLC) {
        rgb_mode = CONFIG_GROOVY_STREAM_RGB_MODE_RGB888;
    }

    uint16_t mtu = g_config.groovy.stream.mtu == CONFIG_GROOVY_STREAM_MTU_3800
                       ? 3800 : 1500;

    /*
     * The emulated audio processor already produces 48 kHz stereo signed
     * 16-bit, which is what the display wants, so nothing is resampled or
     * reformatted anywhere in this path.
     */
    bool want_audio = g_config.groovy.sound.enable;
    uint32_t sound_rate = want_audio ? 48000 : 0;
    uint8_t sound_chan = want_audio ? 2 : 0;

    int rc = g_s.gm->CmdInit(g_config.groovy.host, g_config.groovy.port,
                             g_config.groovy.stream.codec,
                             sound_rate, sound_chan, rgb_mode, mtu);
    if (rc != 0) {
        g_s.gm.reset();
        return false;
    }

    g_s.connected = true;
    g_s.last_send_ms = now_ms();
    g_s.init_codec = g_config.groovy.stream.codec;
    g_s.init_rgb_mode = rgb_mode;
    g_s.init_mtu = mtu;
    g_s.init_audio = want_audio;
    g_s.init_near = near_level();
    g_s.init_pack = g_config.groovy.stream.nlc_pack;
    g_s.epoch = g_s.gm->reconnectEpoch();
    g_s.have_frame = false;
    g_s.audio_primed = false;
    groovy_audio_set_active(want_audio);

    /*
     * What was actually granted, which is not necessarily what was asked for:
     * an older display negotiates down to the simpler controller format with
     * no analog sticks and no rumble. Everything downstream keys off this
     * rather than off the request. The keepalive grant rides in here too, so
     * test single bits rather than the whole byte.
     */
    g_s.caps = g_s.gm->getInputCaps();

    if (g_config.groovy.controls.pads) {
        g_s.gm->ResendInputSubscribe();
        groovy_input_open();
    }
    return true;
}

/*
 * The host's speakers and the display's are a frame or two apart, so hearing
 * both at once combs. Muting leaves the host stream open rather than closing
 * it: the audio processor paces itself against how much audio the host has yet
 * to play, and with no stream at all it falls back to a free-running clock
 * about 60 parts per million fast, which slowly fills the display's buffer
 * over a long session.
 */
float groovy_local_audio_gain(float configured)
{
    if (!groovy_is_streaming() || !g_config.groovy.sound.enable) {
        return configured;
    }

    switch (g_config.groovy.sound.local_output) {
    case CONFIG_GROOVY_SOUND_LOCAL_OUTPUT_ENABLED:
        return configured;
    case CONFIG_GROOVY_SOUND_LOCAL_OUTPUT_DISABLED:
    case CONFIG_GROOVY_SOUND_LOCAL_OUTPUT_MUTED:
    default:
        return 0.0f;
    }
}

static int64_t audio_reserve_bytes(void)
{
    static const uint32_t ms[] = { 0, 16, 32, 64, 128 };
    unsigned int i = g_config.groovy.sound.buffer_ms;
    return (i < std::size(ms) ? ms[i] : 64) * 192;
}

/* A gap the display cannot have measured: its count of missed samples wraps
 * at 65536, so past that its recovery is unknowable and the reserve is simply
 * built again. */
#define AUDIO_GAP_LIMIT (65536 * 4)

static void prime_audio(uint32_t count, GroovyDiagReset reason)
{
    size_t discarded = groovy_audio_reset((uint32_t)audio_reserve_bytes());
    groovy_diag_audio_reset(reason, discarded);
    g_s.audio_last_count = count;
    g_s.audio_carry = 0.0;
    g_s.audio_played = 0;
    g_s.audio_sent = 0;
    g_s.audio_primed = true;
}

/*
 * Advance the display's clock by what it has played since the last look,
 * taken from its frame count. Primed afresh whenever that count restarts,
 * which is what a mode change or a reconnect looks like from here, and
 * whenever the display reports its audio off.
 */
static void clock_audio(void)
{
    if (!g_config.groovy.sound.enable || !g_s.connected) {
        return;
    }

    uint32_t count = g_s.gm->fpga.frame;

    if (!g_s.gm->fpga.audio) {
        g_s.audio_primed = false;
        return;
    }
    if (!g_s.audio_primed || count < g_s.audio_last_count) {
        prime_audio(count, g_s.audio_primed ? GROOVY_DIAG_RESET_CLOCK :
                                            GROOVY_DIAG_RESET_READY);
        return;
    }

    double due = g_s.audio_carry +
                 (count - g_s.audio_last_count) * g_s.audio_bytes_per_count;
    int64_t whole = (int64_t)due;
    g_s.audio_carry = due - (double)whole;
    g_s.audio_last_count = count;
    g_s.audio_played += whole;
    groovy_audio_budget_add((int32_t)whole);
}

/*
 * Top the display's reserve back up to the configured lead.
 *
 * The audio processor is expected to have produced what the display has
 * played; when it has not, the shortfall is sent as silence and taken off its
 * budget rather than left for it to catch up on. Two cases: it was paused
 * with the guest, or the frame loop stopped for longer than the reserve, in
 * which case the display has already played that silence itself and discards
 * as much of what comes next. Silence goes first in the packet so that what
 * it discards is silence.
 */
static void send_audio(void)
{
    if (!g_config.groovy.sound.enable || !g_s.connected || !g_s.audio_primed) {
        return;
    }

    int64_t owed = g_s.audio_played + audio_reserve_bytes() - g_s.audio_sent;
    if (owed <= 0) {
        return;
    }

    int64_t gap = g_s.audio_played - g_s.audio_sent;
    if (gap > AUDIO_GAP_LIMIT) {
        prime_audio(g_s.audio_last_count, GROOVY_DIAG_RESET_GAP);
        return;
    }

    int64_t field = (int64_t)g_s.audio_bytes_per_count;
    int64_t shortfall = -(int64_t)groovy_audio_budget_position() - field;
    int64_t silence = std::max({ gap, shortfall, (int64_t)0 });
    silence = std::min({ silence, owed, (int64_t)GROOVY_AUDIO_MAX_PER_FRAME });
    silence &= ~(int64_t)3;

    char *dst = g_s.gm->getPBufferAudio();
    if (silence) {
        memset(dst, 0, (size_t)silence);
        groovy_audio_budget_add(-(int32_t)silence);
    }

    int64_t room = std::min(owed, (int64_t)GROOVY_AUDIO_MAX_PER_FRAME) - silence;
    size_t avail = std::min(groovy_audio_available(), (size_t)room) & ~(size_t)3;
    size_t got = avail ? groovy_audio_pop(dst + silence, avail) : 0;

    size_t total = (size_t)silence + got;
    if (!total) {
        return;
    }
    g_s.audio_sent += total;
    g_s.gm->CmdAudio((uint16_t)total);
    groovy_diag_audio_sent(got, (size_t)silence);
}

/* Re-resolve when the guest's mode or any setting that shapes the answer has
 * changed, and tell the core about it. */
static void refresh_mode(void)
{
    GroovyModeKey key;
    if (!groovy_modes_current_key(&key)) {
        return;
    }
    if (g_s.have_key && key == g_s.key) {
        return;
    }

    GroovyMode mode;
    char why[192];
    if (!groovy_modes_resolve(key, &mode, why, sizeof(why))) {
        g_s.mode_valid = false;
        char msg[224];
        snprintf(msg, sizeof(msg), "MiSTer: %s", why);
        xemu_queue_notification(msg);
        g_s.key = key;
        g_s.have_key = true;
        return;
    }

    if (!apply_mode(mode, mode.substituted ? why : nullptr)) {
        teardown("MiSTer: the display did not accept the video mode");
        return;
    }

    g_s.key = key;
    g_s.have_key = true;
}

void groovy_update_session(void)
{
    if (!g_owner_thread_known) {
        g_owner_thread = SDL_GetCurrentThreadID();
        g_owner_thread_known = true;
    }

    bool want = g_config.groovy.enable;

    if (!want) {
        if (groovy_is_streaming()) {
            teardown(nullptr);
        }
        return;
    }

    if (groovy_is_streaming()) {
        /*
         * Everything describing the stream itself is fixed when the session
         * opens -- codec, colour depth, detail level, compression pack, packet
         * size, and whether audio is carried at all. They travel in the connect
         * message and there is no way to revise them afterwards, so a change to
         * any of them means starting over. Left out of this list, a setting
         * appears to do nothing until the session happens to restart.
         */
        uint8_t rgb = g_config.groovy.stream.codec == CONFIG_GROOVY_STREAM_CODEC_NLC
                          ? CONFIG_GROOVY_STREAM_RGB_MODE_RGB888
                          : g_config.groovy.stream.rgb_mode;
        int mtu = g_config.groovy.stream.mtu == CONFIG_GROOVY_STREAM_MTU_3800
                      ? 3800 : 1500;

        if (g_s.init_codec != g_config.groovy.stream.codec ||
            g_s.init_rgb_mode != rgb || g_s.init_mtu != mtu ||
            g_s.init_audio != g_config.groovy.sound.enable ||
            g_s.init_near != near_level() ||
            g_s.init_pack != g_config.groovy.stream.nlc_pack) {
            teardown(nullptr);
        }
    }

    groovy_diag_configure(g_config.groovy.log_level);
    if (g_s.gm) {
        groovy_diag_attach(g_s.gm.get());
    }

    if (!groovy_is_streaming()) {
        if (!groovy_modes_init(g_config.groovy.modeline.monitor,
                               g_config.groovy.modeline.monitor_custom)) {
            g_config.groovy.enable = false;
            printf("[groovy] could not start the modeline engine; streaming off\n");
            fflush(stdout);
            xemu_queue_notification("MiSTer: could not start the modeline engine");
            groovy_diag_finish();
            return;
        }
        printf("[groovy] connecting to %s:%d\n", g_config.groovy.host,
               g_config.groovy.port);
        fflush(stdout);
        if (!connect_session()) {
            groovy_modes_shutdown();
            g_config.groovy.enable = false;
            /*
             * Reported on the console as well as on screen. The usual causes --
             * wrong address, the display not running the right core, a firewall
             * -- all look identical from here, and a user chasing one of them
             * needs something they can read after the fact.
             */
            printf("[groovy] no response from %s:%d; streaming off\n",
                   g_config.groovy.host, g_config.groovy.port);
            fflush(stdout);
            char msg[224];
            snprintf(msg, sizeof(msg),
                     "MiSTer: no response from %s", g_config.groovy.host);
            xemu_queue_notification(msg);
            groovy_diag_finish();
            return;
        }
        g_s.next_frame_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }

    refresh_mode();
}

/*
 * Put whatever the blit buffer holds on the wire as the next frame.
 *
 * The frame number must only ever climb. Realigning upward when the core
 * reports it is ahead of us keeps the raster servo converged after a
 * reconnect; realigning downward would stall the input stream.
 */
static void blit_frame(void)
{
    if (g_s.gm->fpga.frame > g_s.frame) {
        g_s.frame = g_s.gm->fpga.frame;
    }

    g_s.gm->CmdBlit(++g_s.frame, 0, g_config.groovy.timing.vsync_line,
                    (uint32_t)(g_config.groovy.timing.fd_margin_ms * 1000000.0f),
                    0);
    g_s.last_send_ms = now_ms();
}

void groovy_frame_captured(unsigned int texture, bool flip_required)
{
    if (!groovy_capture_enabled()) {
        return;
    }

    int64_t t0 = groovy_diag_begin();
    char *dst = g_s.gm->getPBufferBlit(0);

    /* Send a known picture instead of the emulated one, for checking that the
     * CRT shows the right colours the right way up. */
    static const bool test_pattern = getenv("XEMU_GROOVY_TEST_PATTERN") != nullptr;
    if (test_pattern) {
        groovy_fill_test_pattern(g_s.mode.hactive, g_s.mode.vactive, dst);
    } else if (!groovy_capture_to_buffer(texture, flip_required,
                                         g_s.mode.hactive, g_s.mode.vactive,
                                         dst)) {
        return;
    }

    groovy_diag_stage(GROOVY_DIAG_CAPTURE, t0);
    t0 = groovy_diag_begin();

    g_s.have_frame = true;
    blit_frame();

    groovy_diag_stage(GROOVY_DIAG_SEND, t0);
    groovy_diag_blit(false);
}

int64_t groovy_acquire_budget_ns(void)
{
    return groovy_capture_enabled() ? (int64_t)g_s.period_ns / 2 : -1;
}

void groovy_frame_repeat(void)
{
    if (!groovy_capture_enabled()) {
        return;
    }

    if (!g_s.have_frame) {
        return;
    }

    int64_t t0 = groovy_diag_begin();
    blit_frame();
    groovy_diag_stage(GROOVY_DIAG_SEND, t0);
    groovy_diag_blit(true);
}

/*
 * A floor on the frame period.
 *
 * WaitSync only paces while the display is acknowledging frames. It cannot do
 * so before a session is established, and it stops doing so the moment
 * acknowledgements dry up -- which is not the same as the connection being
 * reported as down, because a client retrying a reconnect still considers
 * itself connected. In both cases it returns immediately, and with nothing
 * holding the loop it runs as fast as frames can be produced and drags the
 * guest's timing along with it.
 *
 * This is not a second limiter competing with the first. When the wait did its
 * job the deadline has already passed and this returns without sleeping; it
 * only takes effect on the frames where nothing else paced us.
 */
static void enforce_frame_deadline(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (g_s.next_frame_ns == 0) {
        g_s.next_frame_ns = now;
    }

    g_s.next_frame_ns += (int64_t)g_s.period_ns;
    groovy_diag_deadline(now - g_s.next_frame_ns);

    if (now < g_s.next_frame_ns) {
        SDL_DelayPrecise(g_s.next_frame_ns - now);
    } else if (now > g_s.next_frame_ns + (int64_t)g_s.period_ns) {
        /*
         * More than a whole frame late. Start again from here rather than
         * firing a burst to catch up: the display scans at its own rate and
         * cannot show frames faster, so a burst is wasted work that also
         * provokes the very stall it is trying to recover from. Observed once
         * on hardware as a recovery running at 70 frames a second against a
         * 59.5 Hz display.
         */
        g_s.next_frame_ns = now;
    }
}

void groovy_frame_step(void)
{
    if (!groovy_is_streaming()) {
        return;
    }

    uint32_t epoch = g_s.gm->reconnectEpoch();
    if (g_s.connected && epoch != g_s.epoch) {
        /* Receiver clocks and input sequence numbers restart together. */
        groovy_diag_reconnect(epoch);
        g_s.epoch = epoch;
        g_s.audio_primed = false;
        g_s.audio_carry = 0.0;
        g_s.audio_played = g_s.audio_sent = 0;
        g_s.gm->joyInputs.joyFrame = 0;
        g_s.gm->joyInputs.joyOrder = 0;
        g_s.gm->ps2Inputs.ps2Frame = 0;
        g_s.gm->ps2Inputs.ps2Order = 0;
        if (g_config.groovy.controls.pads) {
            g_s.gm->ResendInputSubscribe();
        }
    }

    if (g_s.connected && g_s.mode_valid) {
        /*
         * Also the only place the send queue is drained on Windows, so it has
         * to run on every frame, including ones where nothing was blitted.
         */
        int64_t tw = groovy_diag_begin();
        g_s.gm->WaitSync();
        groovy_diag_stage(GROOVY_DIAG_PACE, tw);
    }

    /* After the pacing, which is where the acknowledgement carrying the
     * display's frame count is read. */
    clock_audio();
    send_audio();

    int64_t td = groovy_diag_begin();
    enforce_frame_deadline();
    groovy_diag_stage(GROOVY_DIAG_DEADLINE, td);

    if (g_s.connected && g_config.groovy.controls.pads) {
        /*
         * Read after the pacing rather than before the frame is sent, so the
         * values are as recent as possible when the guest next looks at them.
         * Polling earlier would make every press a frame old.
         */
        g_s.gm->PollInputs();

        /* The controllers are read by the guest under the main lock, so they
         * are written under it too. It is held only for the copy. */
        int64_t ti = groovy_diag_begin();
        xemu_main_loop_lock();
        groovy_diag_stage(GROOVY_DIAG_INPUT_LOCK, ti);
        ti = groovy_diag_begin();
        groovy_input_apply(g_s.gm.get(), g_s.caps);
        groovy_diag_applied();
        xemu_main_loop_unlock();
        groovy_diag_stage(GROOVY_DIAG_INPUT_APPLY, ti);

        groovy_input_send_rumble(g_s.gm.get(), g_s.caps);
    }

    if (g_s.connected && g_s.mode_valid && single_clock_wanted()) {
        /*
         * After the pacing, so the guest starts its next frame at the top of
         * the period and has the whole of it to draw before we sample again.
         */
        qatomic_set(&vblank_externally_driven, true);
        int64_t tv = groovy_diag_begin();
        xemu_process_vblank_now();
        groovy_diag_stage(GROOVY_DIAG_VBLANK, tv);
    } else {
        qatomic_set(&vblank_externally_driven, false);
    }

    if (g_s.connected && g_config.groovy.log_level >= 1) {
        groovy_diag_frame(g_s.frame, g_s.gm->fpga.frame, g_s.gm->fpga.frameEcho,
                          g_s.epoch, (int64_t)g_s.period_ns,
                          g_s.audio_primed ? g_s.audio_sent - g_s.audio_played : 0,
                          groovy_audio_budget_position(), groovy_audio_available(),
                          g_s.gm->fpga.audio);
    }

    /* Measured from the last send, not from whether a mode is resolved: a frame
     * is skipped on a refused capture too, and that is just as silent. */
    if (g_s.connected) {
        uint64_t t = now_ms();
        if (t - g_s.last_send_ms >= KEEPALIVE_INTERVAL_MS) {
            g_s.gm->CmdSendKeepAlive();
            g_s.last_send_ms = t;
        }
    }
}

void groovy_shutdown(void)
{
    teardown(nullptr);
}

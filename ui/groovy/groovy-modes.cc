/*
 * Groovy MiSTer output for xemu — modeline resolution
 *
 * Turns the video mode the guest has programmed into a modeline the MiSTer can
 * scan out on a CRT, using switchres as the timing engine.
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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "groovy.h"
#include "groovy-internal.hh"
#include "groovymister.h"
#include "switchres_wrapper.h"

#include "hw/xbox/nv2a/nv2a.h"
#include "ui/xemu-settings.h"

extern "C" {
#include "ui/xemu-widescreen.h"
}

/* Refresh rates within this much of each other are the same rate. 59.94
 * against 60.000 is 0.1%, and must not look like a mode we failed to get. */
#define REFRESH_TOLERANCE_PCT 1.0

/*
 * Fallback resolutions, largest first. switchres only ever scales a picture
 * up, so it answers a request that is too tall by lowering the refresh rate
 * while keeping the width. For a 720p or 1080i guest mode that produces
 * something both unscannable and far past the client's frame limit, so the
 * width has to come down with the height and the rungs are whole resolutions
 * rather than line counts.
 */
static const struct { uint16_t w, h; } k_ladder[] = {
    { 640, 480 },
    { 640, 400 },
    { 320, 288 },
    { 320, 240 },
};

static const char *const k_monitor_names[] = {
    "arcade_15",  "arcade_15ex", "arcade_15_25", "arcade_15_31",
    "arcade_15_25_31", "arcade_25", "arcade_31", "generic_15",
    "ntsc", "pal", "vesa_480", "vesa_600", "vesa_768", "vesa_1024",
    "custom",
};

static bool g_initialised;
static int g_active_preset = -1;

namespace {
struct CacheEntry {
    GroovyModeKey key;
    GroovyMode mode;
    bool resolved;
};
}
static std::vector<CacheEntry> g_cache;

uint32_t groovy_max_pixels(void)
{
    return BUFFER_SIZE / 3;
}

static const char *monitor_name(int preset)
{
    size_t n = sizeof(k_monitor_names) / sizeof(k_monitor_names[0]);
    return (preset >= 0 && (size_t)preset < n) ? k_monitor_names[preset]
                                               : "arcade_15";
}

bool groovy_modes_init(int monitor_preset, const char *custom_ranges)
{
    if (g_initialised) {
        return true;
    }

    /*
     * sr_init() sets LC_NUMERIC to "C" for the whole process and reads a
     * switchres.ini from the working directory if one is present. Neither is
     * wanted here, but neither is harmful: xemu's own settings code brackets
     * its locale use, and we ship no such ini.
     */
    sr_init();
    sr_set_log_level(0);
    sr_set_monitor(monitor_name(monitor_preset));

    if (monitor_preset >= 0 &&
        !strcmp(monitor_name(monitor_preset), "custom") &&
        custom_ranges && custom_ranges[0]) {
        sr_set_option("crt_range0", custom_ranges);
    }

    /* Written again on every resolve; see groovy_modes_resolve. */
    sr_set_option("interlace", "1");

    if (sr_init_disp("dummy", nullptr) < 0) {
        sr_deinit();
        return false;
    }

    g_active_preset = monitor_preset;
    g_initialised = true;
    g_cache.clear();
    return true;
}

void groovy_modes_shutdown(void)
{
    if (!g_initialised) {
        return;
    }
    sr_deinit();
    g_initialised = false;
    g_active_preset = -1;
    g_cache.clear();
}

bool groovy_modes_rebuild(int monitor_preset, const char *custom_ranges)
{
    groovy_modes_shutdown();
    return groovy_modes_init(monitor_preset, custom_ranges);
}

static float target_refresh_for(uint16_t height)
{
    switch (g_config.groovy.modeline.target_refresh) {
    case CONFIG_GROOVY_MODELINE_TARGET_REFRESH_NTSC_5994:
        return 59.94f;
    case CONFIG_GROOVY_MODELINE_TARGET_REFRESH_EXACT_60:
        return 60.0f;
    case CONFIG_GROOVY_MODELINE_TARGET_REFRESH_PAL_50:
        return 50.0f;
    default:
        /*
         * The Xbox's PAL modes are the only ones drawn at 50 Hz, and they are
         * identifiable by line count: 576 for the full frame, 288 for a single
         * field. Everything else, including the HD modes, is 59.94.
         *
         * This is a guess from geometry because the guest's actual field rate
         * is not observable here: the vblank interrupt is driven by a fixed
         * timer rather than by the CRTC programming. A user whose content
         * disagrees can pin the rate in settings.
         */
        return (height == 576 || height == 288) ? 50.0f : 59.94f;
    }
}

static uint16_t g_fallback_w, g_fallback_h;

void groovy_set_fallback_geometry(unsigned int width, unsigned int height)
{
    g_fallback_w = (uint16_t)width;
    g_fallback_h = (uint16_t)height;
}

bool groovy_modes_current_key(GroovyModeKey *out)
{
    NV2ADisplayGeometry geo;
    nv2a_get_display_geometry(&geo);

    uint16_t width = geo.width;
    uint16_t height = geo.height;
    uint8_t interlaced = geo.interlaced;

    if (width == 0 || height == 0) {
        /* Nothing has been composited yet, or the guest is drawing straight to
         * the framebuffer without the accelerated path. Describe whatever the
         * plain display surface is showing instead. */
        width = g_fallback_w;
        height = g_fallback_h;
        interlaced = 0;
    }

    if (width == 0 || height == 0) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->width = width;
    out->height = height;
    out->src_interlaced = interlaced;
    out->widescreen = xemu_get_widescreen() ? 1 : 0;
    out->target_refresh = target_refresh_for(height);
    out->monitor_preset = g_config.groovy.modeline.monitor;
    out->scan_mode = g_config.groovy.modeline.scan_mode;
    out->mode_priority = g_config.groovy.modeline.mode_priority;
    out->keep_res_limit_pct = g_config.groovy.modeline.keep_resolution_limit;
    out->aspect = g_config.groovy.modeline.aspect;
    return true;
}

static double refresh_error_pct(double granted, double wanted)
{
    if (wanted <= 0.0) {
        return 0.0;
    }
    return (granted / wanted - 1.0) * 100.0;
}

static bool mode_fits_client(const sr_mode &m)
{
    if (m.width <= 0 || m.height <= 0) {
        return false;
    }
    return (uint32_t)m.width * (uint32_t)m.height <= groovy_max_pixels();
}

static void fill_mode(const sr_mode &m, GroovyMode *out, bool substituted)
{
    out->pclock_mhz = (double)m.pclock / 1000000.0;
    out->hactive = m.width;
    out->hbegin = m.hbegin;
    out->hend = m.hend;
    out->htotal = m.htotal;
    out->vactive = m.height;
    out->vbegin = m.vbegin;
    out->vend = m.vend;
    out->vtotal = m.vtotal;
    /*
     * Mode 1 would mean sending one half-height field per blit, but the
     * renderers always hand us a full-height progressive image even for the
     * guest's own interlaced modes, so there are no fields here to send. Mode
     * 2 is the case where the core scans alternating fields out of a
     * progressive framebuffer, which is exactly what we have.
     */
    out->interlace = m.interlace ? 2 : 0;
    /* vfreq already accounts for interlace: it is the field rate, which is
     * the rate the guest has to produce frames at. */
    out->refresh = m.vfreq;
    out->substituted = substituted;
}

static bool probe(int w, int h, double refresh, sr_mode *out)
{
    memset(out, 0, sizeof(*out));
    /* SR_ACTION_ADD | SR_ACTION_FLUSH: computes a modeline without switching
     * to it. Walking the ladder with the applying call would churn the aspect
     * ratio and render target on every rung it passed. */
    return sr_add_mode(w, h, refresh, 0, out) >= 0 && out->width > 0;
}

bool groovy_modes_resolve(const GroovyModeKey &key, GroovyMode *out,
                          char *why, size_t why_size)
{
    if (why && why_size) {
        why[0] = '\0';
    }

    if (!g_initialised || g_active_preset != key.monitor_preset) {
        if (!groovy_modes_rebuild(key.monitor_preset,
                                  g_config.groovy.modeline.monitor_custom)) {
            return false;
        }
    }

    for (const CacheEntry &e : g_cache) {
        if (e.key == key) {
            if (!e.resolved) {
                return false;
            }
            *out = e.mode;
            return true;
        }
    }

    /*
     * Written on every resolve, in both directions. The option maps onto the
     * display object and persists for the rest of the session, so setting only
     * the "off" case would leave interlaced modes disabled permanently and make
     * the setting appear to work in one direction only.
     */
    sr_set_option("interlace",
                  key.scan_mode == CONFIG_GROOVY_MODELINE_SCAN_MODE_PROGRESSIVE_ONLY
                      ? "0" : "1");

    bool ok = false;
    bool substituted = false;
    sr_mode chosen = {};

    sr_mode native = {};
    bool have_native = probe(key.width, key.height, key.target_refresh, &native);
    double native_err = have_native
                            ? refresh_error_pct(native.vfreq, key.target_refresh)
                            : 0.0;
    bool native_fits = have_native && mode_fits_client(native);

    bool keep_resolution =
        key.mode_priority == CONFIG_GROOVY_MODELINE_MODE_PRIORITY_KEEP_RESOLUTION;

    if (native_fits) {
        double limit = key.keep_res_limit_pct;
        bool within_rate = fabs(native_err) <= REFRESH_TOLERANCE_PCT;

        if (within_rate) {
            chosen = native;
            ok = true;
        } else if (keep_resolution && (limit == 0.0 || fabs(native_err) <= limit)) {
            /* The user asked to keep the resolution and accepted this much
             * slowdown. The emulated audio hardware runs on real time rather
             * than on the video clock, so this is heard as well as seen. */
            chosen = native;
            ok = true;
            substituted = true;
            if (why && why_size) {
                snprintf(why, why_size,
                         "%ux%u at %.3f Hz (%+.1f%% off %.2f Hz)",
                         native.width, native.height, native.vfreq,
                         native_err, (double)key.target_refresh);
            }
        }
    }

    if (!ok) {
        /* Walk down to something the monitor can scan at the rate the guest
         * needs, skipping rungs that are not actually smaller. */
        for (size_t i = 0; i < sizeof(k_ladder) / sizeof(k_ladder[0]); i++) {
            if (k_ladder[i].h >= key.height && k_ladder[i].w >= key.width) {
                continue;
            }
            sr_mode m = {};
            if (!probe(k_ladder[i].w, k_ladder[i].h, key.target_refresh, &m)) {
                continue;
            }
            if (!mode_fits_client(m)) {
                continue;
            }
            if (fabs(refresh_error_pct(m.vfreq, key.target_refresh)) >
                REFRESH_TOLERANCE_PCT) {
                continue;
            }
            chosen = m;
            ok = true;
            substituted = true;
            if (why && why_size) {
                snprintf(why, why_size,
                         "%ux%u unavailable on this monitor; using %ux%u at %.3f Hz",
                         key.width, key.height, m.width, m.height, m.vfreq);
            }
            break;
        }
    }

    if (!ok && native_fits) {
        /* Nothing matched the rate. Keeping the picture at the wrong rate is
         * still better than no picture, but say so. */
        chosen = native;
        ok = true;
        substituted = true;
        if (why && why_size) {
            snprintf(why, why_size,
                     "no mode at %.2f Hz; using %ux%u at %.3f Hz (%+.1f%%)",
                     (double)key.target_refresh, native.width, native.height,
                     native.vfreq, native_err);
        }
    }

    CacheEntry entry;
    entry.key = key;
    entry.resolved = ok;
    if (ok) {
        fill_mode(chosen, &entry.mode, substituted);
        *out = entry.mode;
    } else {
        memset(&entry.mode, 0, sizeof(entry.mode));
        if (why && why_size && !why[0]) {
            snprintf(why, why_size,
                     "no usable modeline for %ux%u at %.2f Hz",
                     key.width, key.height, (double)key.target_refresh);
        }
    }
    g_cache.push_back(entry);

    return ok;
}

bool groovy_selftest(void)
{
    GroovyMister gm;
    printf("[groovy] client version %s, max %u pixels at RGB888\n",
           gm.getVersion(), groovy_max_pixels());

    if (!groovy_modes_init(g_config.groovy.modeline.monitor,
                           g_config.groovy.modeline.monitor_custom)) {
        printf("[groovy] selftest FAILED: modeline engine did not start\n");
        return false;
    }

    static const struct {
        const char *name;
        uint16_t w, h;
        uint8_t interlaced;
    } probes[] = {
        { "480p",  640,  480,  0 },
        { "480i",  720,  480,  1 },
        { "240p",  320,  240,  0 },
        { "720p",  1280, 720,  0 },
        { "1080i", 1920, 1080, 1 },
    };

    bool ok = true;
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        GroovyModeKey key;
        memset(&key, 0, sizeof(key));
        key.width = probes[i].w;
        key.height = probes[i].h;
        key.src_interlaced = probes[i].interlaced;
        key.target_refresh = target_refresh_for(probes[i].h);
        key.monitor_preset = g_config.groovy.modeline.monitor;
        key.scan_mode = g_config.groovy.modeline.scan_mode;
        key.mode_priority = g_config.groovy.modeline.mode_priority;
        key.keep_res_limit_pct = g_config.groovy.modeline.keep_resolution_limit;
        key.aspect = g_config.groovy.modeline.aspect;

        GroovyMode mode;
        char why[160];
        if (groovy_modes_resolve(key, &mode, why, sizeof(why))) {
            printf("[groovy] %-5s %4ux%-4u -> %4ux%-4u @%7.3f %s%s%s\n",
                   probes[i].name, probes[i].w, probes[i].h,
                   mode.hactive, mode.vactive, mode.refresh,
                   mode.interlace ? "interlaced" : "progressive",
                   why[0] ? " | " : "", why);
        } else {
            printf("[groovy] %-5s %4ux%-4u -> unresolved: %s\n",
                   probes[i].name, probes[i].w, probes[i].h, why);
            ok = false;
        }
    }

    groovy_modes_shutdown();
    printf("[groovy] selftest %s\n", ok ? "passed" : "FAILED");
    return ok;
}

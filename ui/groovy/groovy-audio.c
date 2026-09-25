/*
 * Groovy MiSTer output for xemu — audio hand-off
 *
 * Carries mixed PCM from the audio processor's own thread to the thread that
 * paces frames onto the wire. The producer must never be delayed here: it is
 * pacing real-time audio, and anything that blocks it is heard.
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
#include "qemu/atomic.h"
#include "qemu/timer.h"

#include "groovy.h"
#include "groovy-diagnostics.h"

/*
 * A single producer and a single consumer, so the write index is only ever
 * advanced by the audio thread and the read index only by the frame loop.
 * Neither needs a lock.
 *
 * 64 KiB is about a third of a second at 48 kHz stereo, far more than the few
 * milliseconds normally in flight. It is sized for the pauses -- a mode change,
 * a stalled frame loop -- rather than for steady state.
 */
#define RING_BYTES (64 * 1024)
#define RING_MASK  (RING_BYTES - 1)

static uint8_t g_ring[RING_BYTES];
static uint32_t g_write;    /* written by the audio thread only */
static uint32_t g_read;     /* written by the frame loop only */
static bool g_active;

static uint32_t g_dropped_bytes;
static uint32_t g_starved_frames;

/*
 * How far the audio thread may write, in the same terms as the write index.
 * Advanced by the frame loop from the display's frame count. Left alone for a
 * quarter of a second it lapses, so a frame loop that has stopped cannot hold
 * the guest's audio with it.
 */
static uint32_t g_budget;
static int64_t g_budget_ns;     /* when it was last advanced */

#define BUDGET_LAPSE_NS 250000000
/* Slack past the budget before the producer is held. */
#define BUDGET_SLACK 4096
/* Kept free however far behind the budget the producer is, so it stops short
 * of a full ring rather than dropping into it. */
#define RING_HEADROOM 8192

void groovy_audio_set_active(bool active)
{
    if (active && !qatomic_read(&g_active)) {
        /* Start from silence rather than from whatever was left behind by a
         * previous session. */
        qatomic_set(&g_read, qatomic_read(&g_write));
    }
    if (!active) {
        qatomic_set(&g_budget_ns, 0);
    }
    qatomic_set(&g_active, active);
}

bool groovy_audio_active(void)
{
    return qatomic_read(&g_active);
}

void groovy_audio_push(const void *pcm, size_t bytes)
{
    if (!qatomic_read(&g_active) || !pcm || !bytes) {
        return;
    }

    uint32_t write = qatomic_read(&g_write);
    uint32_t read = qatomic_read(&g_read);
    uint32_t used = write - read;
    uint32_t space = RING_BYTES - used;

    if (bytes > space) {
        /*
         * The consumer drains several times faster than this fills, so a full
         * ring means the frame loop has stopped rather than fallen behind.
         * Discard and count it: blocking here would stall the audio processor,
         * and there is nothing useful to wait for.
         */
        groovy_diag_audio_produced(bytes, true);
        qatomic_set(&g_dropped_bytes, qatomic_read(&g_dropped_bytes) + bytes);
        return;
    }

    const uint8_t *src = pcm;
    uint32_t offset = write & RING_MASK;
    uint32_t first = RING_BYTES - offset;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(&g_ring[offset], src, first);
    if (bytes > first) {
        memcpy(&g_ring[0], src + first, bytes - first);
    }

    /* Publish only once the bytes are in place. */
    smp_wmb();
    qatomic_set(&g_write, write + (uint32_t)bytes);
    groovy_diag_audio_produced(bytes, false);
}

size_t groovy_audio_available(void)
{
    return qatomic_read(&g_write) - qatomic_read(&g_read);
}

size_t groovy_audio_pop(void *dst, size_t bytes)
{
    uint32_t read = qatomic_read(&g_read);
    uint32_t avail = qatomic_read(&g_write) - read;

    if (bytes > avail) {
        bytes = avail;
    }
    if (!bytes) {
        qatomic_set(&g_starved_frames, qatomic_read(&g_starved_frames) + 1);
        return 0;
    }
    smp_rmb();

    uint8_t *out = dst;
    uint32_t offset = read & RING_MASK;
    uint32_t first = RING_BYTES - offset;
    if (first > bytes) {
        first = (uint32_t)bytes;
    }
    memcpy(out, &g_ring[offset], first);
    if (bytes > first) {
        memcpy(out + first, &g_ring[0], bytes - first);
    }

    qatomic_set(&g_read, read + (uint32_t)bytes);
    return bytes;
}

void groovy_audio_stats(uint32_t *dropped_bytes, uint32_t *starved_frames)
{
    if (dropped_bytes) {
        *dropped_bytes = qatomic_read(&g_dropped_bytes);
    }
    if (starved_frames) {
        *starved_frames = qatomic_read(&g_starved_frames);
    }
}

size_t groovy_audio_reset(uint32_t lead_bytes)
{
    uint32_t write = qatomic_read(&g_write);
    uint32_t discarded = write - qatomic_read(&g_read);

    /* Use one producer snapshot for both the queue and its new clock. */
    qatomic_set(&g_read, write);
    qatomic_set(&g_budget, write + lead_bytes);
    qatomic_set(&g_budget_ns, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
    return discarded;
}

void groovy_audio_budget_add(int32_t bytes)
{
    qatomic_set(&g_budget, qatomic_read(&g_budget) + (uint32_t)bytes);
    qatomic_set(&g_budget_ns, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
}

int32_t groovy_audio_budget_position(void)
{
    return (int32_t)(qatomic_read(&g_write) - qatomic_read(&g_budget));
}

bool groovy_audio_pacing(int *queued, int *low, int *high)
{
    if (!qatomic_read(&g_active)) {
        return false;
    }

    int64_t stamped = qatomic_read(&g_budget_ns);
    if (stamped == 0 ||
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - stamped > BUDGET_LAPSE_NS) {
        return false;
    }

    uint32_t write = qatomic_read(&g_write);
    int32_t ahead = (int32_t)(write - qatomic_read(&g_budget));
    int32_t crowded = (int32_t)(write - qatomic_read(&g_read)) -
                      (RING_BYTES - RING_HEADROOM);

    *queued = MAX(ahead, crowded);
    *low = 0;
    *high = BUDGET_SLACK;
    return true;
}

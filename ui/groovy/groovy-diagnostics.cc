/*
 * Groovy MiSTer streaming diagnostics
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

#include "qemu/osdep.h"
#include "groovy-diagnostics.h"
#include "groovymister.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <iterator>

extern "C" {
#include "qemu/atomic.h"
#include "qemu/timer.h"
extern uint64_t vblank_count_timer;
extern uint64_t vblank_count_external;
}

namespace {

enum EventType { FRAME, AUDIO, PACKET, MAPPED, TRANSPORT, BLIT, DECODED,
                 SERVICE, RECONNECT, AUDIO_RESET };
struct Event {
    int64_t ns;
    EventType type;
    int64_t data[GROOVY_DIAG_STAGE_COUNT + 3];
};
constexpr size_t TRACE_CAPACITY = 131072;
Event *g_trace;
uint64_t g_events;
constexpr size_t ANOMALY_CAPACITY = 1024;
Event *g_anomalies;
uint64_t g_anomaly_events;
int64_t g_poll_start, g_nested_service, g_last_service_start, g_period;
int64_t g_last_frame[GROOVY_DIAG_STAGE_COUNT + 3], g_last_audio[12];
int64_t g_service_max, g_service_interval_max;
uint64_t g_recovery_discarded, g_audio_resets, g_reconnects;
int g_level;
bool g_producer_enabled;
SDL_ThreadID g_owner;
int64_t g_origin;
int64_t g_stages[GROOVY_DIAG_STAGE_COUNT];
int64_t g_stage_sum[GROOVY_DIAG_STAGE_COUNT];
int64_t g_stage_max[GROOVY_DIAG_STAGE_COUNT];
uint64_t g_frames, g_fresh, g_repeats, g_received, g_accepted, g_stale, g_other;
uint64_t g_sent_pcm, g_silence, g_errors, g_late_count;
uint64_t g_pcm_total, g_silence_total;
uint64_t g_previous_timer, g_previous_external;
unsigned int g_generation;
int64_t g_report_ns, g_last_send, g_send_gap, g_last_ack, g_ack_age;
int64_t g_first_input, g_apply_delay, g_late_max;
uint32_t g_echo;
bool g_have_echo;
int64_t g_last_mapped[2][8];
bool g_have_mapped[2];

/* Only the APU writes these; the owner samples without blocking it. */
uint64_t g_produced, g_dropped;
int64_t g_last_produced;
std::atomic<int64_t> g_produce_gap;
uint64_t g_previous_produced, g_previous_dropped;

int64_t clock_ns()
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

bool enabled()
{
    return qatomic_read(&g_producer_enabled) &&
           SDL_GetCurrentThreadID() == g_owner;
}

void record(EventType type, const int64_t *data, size_t count, int64_t ns)
{
    if (g_level < 2 || !g_trace) {
        return;
    }
    Event &event = g_trace[g_events++ % TRACE_CAPACITY];
    event = {};
    event.ns = ns - g_origin;
    event.type = type;
    memcpy(event.data, data, count * sizeof(*data));
}

void record_anomaly(EventType type, const int64_t *data, size_t count, int64_t ns)
{
    if (g_level < 2 || !g_anomalies) {
        return;
    }
    Event &event = g_anomalies[g_anomaly_events++ % ANOMALY_CAPACITY];
    event = {};
    event.ns = ns - g_origin;
    event.type = type;
    memcpy(event.data, data, count * sizeof(*data));
}

void print_event(const char *prefix, const Event &event)
{
    static const char *names[] = { "frame", "audio", "packet", "mapped",
        "transport", "blit", "decoded", "service", "reconnect", "audio_reset" };
    printf("[%s] %" PRId64 " %s", prefix, event.ns, names[event.type]);
    if (event.type == PACKET) {
        printf(" %" PRId64 " %" PRId64 " ", event.data[0], event.data[1]);
        const uint8_t *bytes = (const uint8_t *)(event.data + 2);
        for (int j = 0; j < std::min<int64_t>(event.data[1], 41); j++) {
            printf("%02x", bytes[j]);
        }
    } else {
        static const int counts[] = { GROOVY_DIAG_STAGE_COUNT + 3, 12, 0, 9,
                                     2, 1, 10, 3, 1, 2 };
        for (int j = 0; j < counts[event.type]; j++) {
            printf(" %" PRId64, event.data[j]);
        }
    }
    printf("\n");
}

void observe(void *, gm_observation kind, const void *data, int size, int status)
{
    if (!enabled()) {
        return;
    }
    if (kind == GM_INPUT_DECODED && g_level < 2) {
        return;
    }
    int64_t now = clock_ns();
    if (kind == GM_ACK) {
        uint32_t echo;
        memcpy(&echo, data, sizeof(echo));
        if (!g_have_echo || echo != g_echo) {
            g_last_ack = now;
            g_echo = echo;
            g_have_echo = true;
        }
        return;
    }
    if (kind == GM_INPUT_DECODED) {
        const fpgaJoyInputs &j = *(const fpgaJoyInputs *)data;
        int64_t pads[2][10] = {
            { 0, j.joyFrame, j.joyOrder, j.joy1, (int8_t)j.joy1LXAnalog,
              (int8_t)j.joy1LYAnalog, (int8_t)j.joy1RXAnalog, (int8_t)j.joy1RYAnalog,
              j.joy1LTAnalog, j.joy1RTAnalog },
            { 1, j.joyFrame, j.joyOrder, j.joy2, (int8_t)j.joy2LXAnalog,
              (int8_t)j.joy2LYAnalog, (int8_t)j.joy2RXAnalog, (int8_t)j.joy2RYAnalog,
              j.joy2LTAnalog, j.joy2RTAnalog }
        };
        record(DECODED, pads[0], 10, now);
        record(DECODED, pads[1], 10, now);
    } else if (kind <= GM_INPUT_OTHER) {
        g_received++;
        if (kind == GM_INPUT_ACCEPTED) {
            g_accepted++;
            if (!g_first_input) {
                g_first_input = now;
            }
        } else if (kind == GM_INPUT_STALE) {
            g_stale++;
        } else {
            g_other++;
        }
        int64_t values[12] = { kind, size };
        /* Preserve bytes before decoding, including unknown packet formats. */
        memcpy(values + 2, data, std::min(size, 41));
        record(PACKET, values, 12, now);
    } else {
        g_errors++;
        int64_t values[] = { kind, status };
        record(TRANSPORT, values, 2, now);
    }
}

} // namespace

void groovy_diag_configure(int level)
{
    level = std::max(0, std::min(level, 2));
    if (level == g_level) {
        return;
    }
    qatomic_set(&g_producer_enabled, false);
    g_owner = SDL_GetCurrentThreadID();
    g_level = level;
    if (!g_origin) {
        g_origin = clock_ns();
    }
    if (level >= 2 && !g_trace) {
        g_trace = g_try_new(Event, TRACE_CAPACITY);
        if (!g_trace) {
            fprintf(stderr, "[groovy] Could not allocate diagnostic trace\n");
        }
    }
    if (level >= 2 && !g_anomalies) {
        g_anomalies = g_try_new(Event, ANOMALY_CAPACITY);
        if (!g_anomalies) {
            fprintf(stderr, "[groovy] Could not allocate anomaly history\n");
        }
    }
    g_poll_start = g_nested_service = g_last_service_start = g_period = 0;
    g_service_max = g_service_interval_max = 0;
    g_recovery_discarded = g_audio_resets = g_reconnects = 0;
    memset(g_stages, 0, sizeof(g_stages));
    memset(g_stage_sum, 0, sizeof(g_stage_sum));
    memset(g_stage_max, 0, sizeof(g_stage_max));
    g_frames = g_fresh = g_repeats = g_received = g_accepted = g_stale = g_other = 0;
    g_sent_pcm = g_silence = g_errors = g_late_count = 0;
    g_send_gap = g_apply_delay = g_late_max = g_ack_age = 0;
    g_last_send = g_first_input = 0;
    g_have_echo = false;
    g_report_ns = g_last_ack = clock_ns();
    g_previous_produced = qatomic_read(&g_produced);
    g_previous_dropped = qatomic_read(&g_dropped);
    g_previous_timer = qatomic_read__nocheck(&vblank_count_timer);
    g_previous_external = qatomic_read__nocheck(&vblank_count_external);
    qatomic_set(&g_generation, qatomic_read(&g_generation) + 1);
    g_produce_gap.exchange(0, std::memory_order_relaxed);
    qatomic_set(&g_producer_enabled, level > 0);
}

void groovy_diag_attach(GroovyMister *gm)
{
    gm->setObserver(g_level ? observe : nullptr, nullptr);
    /* The buffered trace replaces the client's synchronous packet logging. */
    gm->setVerbose(0);
}

int64_t groovy_diag_begin(void)
{
    return enabled() ? clock_ns() : 0;
}

void groovy_diag_stage(GroovyDiagStage stage, int64_t start)
{
    if (start && enabled()) {
        g_stages[stage] += clock_ns() - start;
    }
}

void groovy_diag_poll_begin(void)
{
    if (enabled()) {
        g_poll_start = clock_ns();
        g_nested_service = 0;
    }
}

void groovy_diag_poll_end(void)
{
    if (enabled() && g_poll_start) {
        g_stages[GROOVY_DIAG_EVENTS] += clock_ns() - g_poll_start - g_nested_service;
        g_poll_start = 0;
    }
}

void groovy_diag_service(int64_t start, bool live_resize)
{
    if (!start || !enabled()) {
        return;
    }
    int64_t now = clock_ns();
    int64_t duration = now - start;
    int64_t interval = g_last_service_start ? start - g_last_service_start : 0;
    g_last_service_start = start;
    if (g_poll_start) {
        g_nested_service += duration;
    }
    g_service_max = std::max(g_service_max, duration);
    g_service_interval_max = std::max(g_service_interval_max, interval);
    int64_t values[] = { duration, interval, live_resize };
    record(SERVICE, values, 3, now);
    if (g_period && std::max(duration, interval) > 2 * g_period) {
        record_anomaly(SERVICE, values, 3, now);
        record_anomaly(FRAME, g_last_frame, std::size(g_last_frame), now);
        record_anomaly(AUDIO, g_last_audio, std::size(g_last_audio), now);
    }
}

void groovy_diag_reconnect(uint32_t epoch)
{
    if (enabled()) {
        g_reconnects++;
        int64_t value = epoch;
        int64_t now = clock_ns();
        record(RECONNECT, &value, 1, now);
        record_anomaly(RECONNECT, &value, 1, now);
    }
}

void groovy_diag_audio_reset(GroovyDiagReset reason, size_t discarded)
{
    if (enabled()) {
        g_audio_resets++;
        g_recovery_discarded += discarded;
        int64_t values[] = { reason, (int64_t)discarded };
        int64_t now = clock_ns();
        record(AUDIO_RESET, values, 2, now);
        record_anomaly(AUDIO_RESET, values, 2, now);
    }
}

void groovy_diag_blit(bool repeat)
{
    if (enabled()) {
        int64_t value = repeat;
        record(BLIT, &value, 1, clock_ns());
        if (repeat) {
            g_repeats++;
        } else {
            g_fresh++;
        }
    }
}

void groovy_diag_audio_produced(size_t bytes, bool dropped)
{
    if (!qatomic_read(&g_producer_enabled)) {
        return;
    }
    if (dropped) {
        qatomic_set(&g_dropped, qatomic_read(&g_dropped) + bytes);
        return;
    }
    int64_t now = clock_ns();
    static unsigned int generation;
    unsigned int current = qatomic_read(&g_generation);
    int64_t previous = generation == current ? qatomic_read(&g_last_produced) : 0;
    generation = current;
    if (previous) {
        int64_t gap = now - previous;
        int64_t peak = g_produce_gap.load(std::memory_order_relaxed);
        while (gap > peak && !g_produce_gap.compare_exchange_weak(
                   peak, gap, std::memory_order_relaxed)) {
        }
    }
    qatomic_set(&g_last_produced, now);
    qatomic_set(&g_produced, qatomic_read(&g_produced) + bytes);
}

void groovy_diag_audio_sent(size_t pcm, size_t silence)
{
    if (!enabled()) {
        return;
    }
    int64_t now = clock_ns();
    if (g_last_send) {
        g_send_gap = std::max(g_send_gap, now - g_last_send);
    }
    g_last_send = now;
    g_sent_pcm += pcm;
    g_silence += silence;
    g_pcm_total += pcm;
    g_silence_total += silence;
}

void groovy_diag_mapped(int pad, int port, uint16_t buttons,
                        const int16_t axes[6])
{
    if (!enabled() || g_level < 2) {
        return;
    }
    int64_t values[] = { pad, port, buttons, axes[0], axes[1], axes[2],
                         axes[3], axes[4], axes[5] };
    if (!g_have_mapped[pad] ||
        memcmp(g_last_mapped[pad], values + 1, sizeof(g_last_mapped[pad]))) {
        g_have_mapped[pad] = true;
        memcpy(g_last_mapped[pad], values + 1, sizeof(g_last_mapped[pad]));
        record(MAPPED, values, 9, clock_ns());
    }
}

void groovy_diag_applied(void)
{
    if (enabled() && g_first_input) {
        g_apply_delay = std::max(g_apply_delay, clock_ns() - g_first_input);
        g_first_input = 0;
    }
}

void groovy_diag_deadline(int64_t late_ns)
{
    if (enabled() && late_ns > 0) {
        g_late_count++;
        g_late_max = std::max(g_late_max, late_ns);
    }
}

void groovy_diag_frame(uint32_t frame, uint32_t display, uint32_t echo,
                       uint32_t epoch, int64_t period_ns, int64_t reserve_bytes,
                       int32_t budget_bytes, size_t queued, bool audio)
{
    if (!enabled()) {
        return;
    }
    int64_t now = clock_ns();
    int64_t values[GROOVY_DIAG_STAGE_COUNT + 3];
    for (int i = 0; i < GROOVY_DIAG_STAGE_COUNT; i++) {
        values[i] = g_stages[i];
        g_stage_sum[i] += g_stages[i];
        g_stage_max[i] = std::max(g_stage_max[i], g_stages[i]);
        g_stages[i] = 0;
    }
    values[GROOVY_DIAG_STAGE_COUNT] = frame;
    values[GROOVY_DIAG_STAGE_COUNT + 1] = period_ns;
    values[GROOVY_DIAG_STAGE_COUNT + 2] = epoch;
    g_period = period_ns;
    memcpy(g_last_frame, values, sizeof(values));
    record(FRAME, values, std::size(values), now);
    uint64_t produced = qatomic_read(&g_produced);
    uint64_t dropped = qatomic_read(&g_dropped);
    int64_t audio_values[] = { (int64_t)produced, (int64_t)dropped,
        (int64_t)g_pcm_total, (int64_t)g_silence_total, reserve_bytes, budget_bytes,
        (int64_t)queued, display, echo, audio,
        now - g_last_ack, g_apply_delay };
    memcpy(g_last_audio, audio_values, sizeof(audio_values));
    record(AUDIO, audio_values, 12, now);
    g_ack_age = std::max(g_ack_age, now - g_last_ack);
    g_frames++;
    if (audio) {
        g_send_gap = std::max(g_send_gap, now - (g_last_send ? g_last_send : g_report_ns));
    }
    if (now - g_report_ns < 5000000000LL) {
        return;
    }
    double seconds = (now - g_report_ns) / 1e9;
    printf("[groovy] %.1fs: %.1f fresh and %.1f repeated blit submissions/s; "
           "deadline late %" PRIu64 " (max %.2f ms), ACK age max %.2f ms\n",
           seconds, g_fresh / seconds, g_repeats / seconds, g_late_count,
           g_late_max / 1e6, g_ack_age / 1e6);
    uint64_t timer = qatomic_read__nocheck(&vblank_count_timer);
    uint64_t external = qatomic_read__nocheck(&vblank_count_external);
    printf("[groovy]   vblank: loop=%.1f/s timer=%.1f/s\n",
           (external - g_previous_external) / seconds,
           (timer - g_previous_timer) / seconds);
    g_previous_timer = timer;
    g_previous_external = external;
    printf("[groovy]   input packets: received=%" PRIu64 " accepted=%" PRIu64
           " stale=%" PRIu64 " other=%" PRIu64 "; receive-to-apply max %.2f ms\n",
           g_received, g_accepted, g_stale, g_other, g_apply_delay / 1e6);
    int64_t produce_gap = g_produce_gap.exchange(0, std::memory_order_relaxed);
    int64_t last_produced = qatomic_read(&g_last_produced);
    if (last_produced >= g_report_ns) {
        produce_gap = std::max(produce_gap, now - last_produced);
    } else {
        produce_gap = std::max(produce_gap, now - g_report_ns);
    }
    printf("[groovy]   audio bytes: produced=%" PRIu64 " discarded=%" PRIu64
           " submitted_pcm=%" PRIu64 " inserted_silence=%" PRIu64
           "; gaps production=%.2f submission=%.2f ms; transport errors=%" PRIu64 "\n",
           produced - g_previous_produced, dropped - g_previous_dropped,
           g_sent_pcm, g_silence, produce_gap / 1e6, g_send_gap / 1e6, g_errors);
    printf("[groovy]   audio reserve estimated %.1f ms, producer %+.1f ms, "
           "queued %zu bytes, receiver audio %s\n",
           reserve_bytes / 192.0, budget_bytes / 192.0, queued, audio ? "on" : "off");
    printf("[groovy]   recovery: reconnects=%" PRIu64 " audio_resets=%" PRIu64
           " discarded_bytes=%" PRIu64 "; service max %.2f ms, interval max %.2f ms\n",
           g_reconnects, g_audio_resets, g_recovery_discarded,
           g_service_max / 1e6, g_service_interval_max / 1e6);
    static const char *names[] = { "acquire", "renderer_lock", "fifo_lock",
        "sync", "capture", "send", "present", "pace", "deadline_wait",
        "events", "session", "input_lock", "input_apply", "vblank" };
    printf("[groovy]   stages mean/max ms:");
    for (int i = 0; i < GROOVY_DIAG_STAGE_COUNT; i++) {
        printf(" %s=%.2f/%.2f", names[i],
               g_stage_sum[i] / (double)g_frames / 1e6, g_stage_max[i] / 1e6);
    }
    printf("\n");
    g_previous_produced = produced;
    g_previous_dropped = dropped;
    g_frames = g_fresh = g_repeats = g_received = g_accepted = g_stale = g_other = 0;
    g_sent_pcm = g_silence = g_errors = g_late_count = 0;
    g_send_gap = g_apply_delay = g_late_max = g_ack_age = 0;
    memset(g_stage_sum, 0, sizeof(g_stage_sum));
    memset(g_stage_max, 0, sizeof(g_stage_max));
    g_service_max = g_service_interval_max = 0;
    g_recovery_discarded = g_audio_resets = g_reconnects = 0;
    g_report_ns = now;
}

void groovy_diag_finish(void)
{
    qatomic_set(&g_producer_enabled, false);
    if (g_trace || g_anomalies) {
        uint64_t first = g_events > TRACE_CAPACITY ? g_events - TRACE_CAPACITY : 0;
        printf("[groovy-trace] version=2 overwritten=%" PRIu64 " time=ns\n", first);
        printf("[groovy-trace] frame: acquire renderer_lock fifo_lock sync capture "
               "send present pace deadline_wait events session input_lock input_apply "
               "vblank frame period epoch\n");
        printf("[groovy-trace] audio: produced_total discarded_total pcm_total "
               "silence_total reserve_bytes budget_bytes queued display echo "
               "enabled ack_age_ns input_apply_max_ns\n");
        printf("[groovy-trace] packet: disposition(0=accepted,1=stale,2=other) "
               "length hex_bytes\n");
        printf("[groovy-trace] mapped: pad port buttons lx ly rx ry lt rt\n");
        printf("[groovy-trace] transport: kind(4=send,5=completion,6=receive) status\n");
        printf("[groovy-trace] blit: repeated\n");
        printf("[groovy-trace] decoded: pad frame order buttons lx ly rx ry lt rt\n");
        printf("[groovy-trace] service: duration_ns interval_ns live_resize\n");
        printf("[groovy-trace] reconnect: epoch\n");
        printf("[groovy-trace] audio_reset: reason(0=ready,1=clock,2=gap) discarded_bytes\n");
        uint64_t first_anomaly = g_anomaly_events > ANOMALY_CAPACITY ?
                                g_anomaly_events - ANOMALY_CAPACITY : 0;
        printf("[groovy-anomaly] overwritten=%" PRIu64 " time=ns\n", first_anomaly);
        for (uint64_t i = first_anomaly; i < g_anomaly_events; i++) {
            print_event("groovy-anomaly", g_anomalies[i % ANOMALY_CAPACITY]);
        }
        printf("[groovy-anomaly] end\n");
        for (uint64_t i = first; i < g_events; i++) {
            print_event("groovy-trace", g_trace[i % TRACE_CAPACITY]);
        }
        printf("[groovy-trace] end\n");
        fflush(stdout);
    }
    g_clear_pointer(&g_trace, g_free);
    g_clear_pointer(&g_anomalies, g_free);
    g_events = g_anomaly_events = 0;
    g_poll_start = g_nested_service = g_last_service_start = g_period = 0;
    g_pcm_total = g_silence_total = 0;
    g_origin = 0;
    g_level = 0;
    memset(g_have_mapped, 0, sizeof(g_have_mapped));
}

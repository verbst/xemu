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

#ifndef XEMU_GROOVY_DIAGNOSTICS_H
#define XEMU_GROOVY_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GroovyDiagStage {
    GROOVY_DIAG_ACQUIRE,
    GROOVY_DIAG_RENDERER_LOCK,
    GROOVY_DIAG_FIFO_LOCK,
    GROOVY_DIAG_SYNC,
    GROOVY_DIAG_CAPTURE,
    GROOVY_DIAG_SEND,
    GROOVY_DIAG_PRESENT,
    GROOVY_DIAG_PACE,
    GROOVY_DIAG_DEADLINE,
    GROOVY_DIAG_EVENTS,
    GROOVY_DIAG_SESSION,
    GROOVY_DIAG_INPUT_LOCK,
    GROOVY_DIAG_INPUT_APPLY,
    GROOVY_DIAG_VBLANK,
    GROOVY_DIAG_STAGE_COUNT
} GroovyDiagStage;

typedef enum GroovyDiagReset {
    GROOVY_DIAG_RESET_READY,
    GROOVY_DIAG_RESET_CLOCK,
    GROOVY_DIAG_RESET_GAP,
} GroovyDiagReset;

/* Owner-thread observations; begin returns zero when diagnostics are off. */
void groovy_diag_configure(int level);
int64_t groovy_diag_begin(void);
void groovy_diag_stage(GroovyDiagStage stage, int64_t start);
void groovy_diag_poll_begin(void);
void groovy_diag_poll_end(void);
void groovy_diag_service(int64_t start, bool live_resize);
void groovy_diag_reconnect(uint32_t epoch);
void groovy_diag_audio_reset(GroovyDiagReset reason, size_t discarded);
void groovy_diag_blit(bool repeat);
void groovy_diag_audio_sent(size_t pcm, size_t silence);
void groovy_diag_mapped(int pad, int port, uint16_t buttons,
                        const int16_t axes[6]);
void groovy_diag_applied(void);
void groovy_diag_deadline(int64_t late_ns);
void groovy_diag_frame(uint32_t frame, uint32_t display, uint32_t echo,
                       uint32_t epoch, int64_t period_ns, int64_t reserve_bytes,
                       int32_t budget_bytes, size_t queued, bool audio);
void groovy_diag_finish(void);

/* Called only by the audio producer; counters are atomic. */
void groovy_diag_audio_produced(size_t bytes, bool dropped);

#ifdef __cplusplus
}

class GroovyMister;
void groovy_diag_attach(GroovyMister *gm);
#endif
#endif

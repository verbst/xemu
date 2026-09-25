/*
 * QEMU Geforce NV2A implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2020-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_NV2A_H
#define HW_NV2A_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The video mode the guest has programmed, as the renderer saw it while
 * compositing the most recent frame.
 *
 * width and height are the guest's own numbers: before the height doubling
 * applied to interlaced modes, and before the surface scale factor. Consumers
 * that need to describe the mode to something outside the emulator, rather
 * than size a render target, want these rather than the dimensions of the
 * texture that comes back from nv2a_get_framebuffer_surface().
 *
 * seq increments whenever any field changes, so a consumer can tell a genuine
 * mode change from a repeated read.
 */
typedef struct NV2ADisplayGeometry {
    uint16_t width;
    uint16_t height;
    uint8_t interlaced;
    uint8_t scale;
    uint32_t seq;
} NV2ADisplayGeometry;

void nv2a_init(PCIBus *bus, int devfn, MemoryRegion *ram);
void nv2a_context_init(void);
int nv2a_get_framebuffer_surface(void);
/*
 * As above, but gives up after timeout_ns and returns -1 with nothing acquired.
 * The composite stays requested and a later call collects it. A negative
 * timeout waits indefinitely.
 */
int nv2a_get_framebuffer_surface_timeout(int64_t timeout_ns);
void nv2a_release_framebuffer_surface(void);
void nv2a_set_surface_scale_factor(unsigned int scale);
unsigned int nv2a_get_surface_scale_factor(void);
const uint8_t *nv2a_get_dac_palette(void);
int nv2a_get_screen_off(void);

/*
 * Read the geometry published by the last composite. Safe to call from any
 * thread; a caller that reads it immediately after nv2a_get_framebuffer_surface()
 * returns is guaranteed the values belonging to that frame, because acquiring
 * the surface waits for the renderer to finish compositing it.
 */
void nv2a_get_display_geometry(NV2ADisplayGeometry *out);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Groovy MiSTer output for xemu — frame capture
 *
 * Reads the composited frame back from the GPU in the byte order and geometry
 * the MiSTer expects.
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

#include <memory>

#include "groovy.h"
#include "groovy-internal.hh"

#include "ui/xui/gl-helpers.hh"

/*
 * Sized to the modeline rather than to the source. The guest's resolution and
 * the resolution the CRT will scan are different numbers, and the scaling
 * between them is a shader blit we would be paying for anyway.
 *
 * Kept between frames: this runs once per frame, and constructing a texture
 * and framebuffer object each time would be a driver allocation per frame.
 */
static std::unique_ptr<Fbo> g_capture_fbo;

void groovy_fill_test_pattern(uint16_t width, uint16_t height, char *dst)
{
    if (!dst || !width || !height) {
        return;
    }

    /* Bytes are blue, green, red in that order. */
    static const uint8_t bands[4][3] = {
        { 255, 255, 255 },  /* white, marks the top edge */
        {   0,   0, 255 },  /* red */
        {   0, 255,   0 },  /* green */
        { 255,   0,   0 },  /* blue */
    };

    uint16_t marker_rows = height / 16 ? height / 16 : 1;
    uint16_t band_rows = (height - marker_rows) / 3;

    for (uint16_t y = 0; y < height; y++) {
        int band;
        if (y < marker_rows) {
            band = 0;
        } else if (band_rows == 0) {
            band = 1;
        } else {
            band = 1 + (y - marker_rows) / band_rows;
            if (band > 3) {
                band = 3;
            }
        }

        char *row = dst + (size_t)y * width * 3;
        for (uint16_t x = 0; x < width; x++) {
            row[x * 3 + 0] = (char)bands[band][0];
            row[x * 3 + 1] = (char)bands[band][1];
            row[x * 3 + 2] = (char)bands[band][2];
        }
    }
}

void groovy_capture_release(void)
{
    g_capture_fbo.reset();
}

bool groovy_capture_to_buffer(unsigned int texture, bool flip_required,
                              uint16_t width, uint16_t height, char *dst)
{
    if (!texture || !dst || !width || !height) {
        return false;
    }

    if (!g_capture_fbo || g_capture_fbo->w != width ||
        g_capture_fbo->h != height) {
        g_capture_fbo.reset(new Fbo(width, height));
    }

    g_capture_fbo->Target();

    bool blend = glIsEnabled(GL_BLEND);
    if (blend) {
        glDisable(GL_BLEND);
    }

    /*
     * The unit scale matters. The other overload derives its scale from the
     * window fit and aspect-ratio settings, which describe how the user wants
     * the picture letterboxed in their window; baking that into the stream
     * would send black bars to the CRT. Aspect on the CRT is the modeline's
     * job.
     */
    float scale[2] = { 1.0f, 1.0f };
    RenderFramebuffer(texture, width, height, !flip_required, scale);

    if (blend) {
        glEnable(GL_BLEND);
    }

    /*
     * The guest's usual surface format is already BGRA, and reads back as BGR
     * with the alpha dropped, which is the byte order the MiSTer's framebuffer
     * wants. No channel swap on the CPU.
     *
     * Row length zero means rows are packed to the width being read, and
     * alignment one keeps them tightly packed for widths that are not a
     * multiple of four.
     */
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, dst);

    g_capture_fbo->Restore();
    return true;
}

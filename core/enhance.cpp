// JagDroid - display enhancements built on top of the emulated picture.
//
// Smooth Gouraud: the blitter interpolates intensity with 16 fractional bits
// but stores only the top 8, so a gently shaded surface shows bands one
// intensity step wide. We keep the next 8 bits per pixel (see Jaguar::shadeTag)
// and rebuild those surfaces at 16-bit intensity, ordered-dithered back to
// 8 bits per channel.
//
// 2x: the Jaguar draws polygons as whole-pixel spans worked out by game code,
// so there is no finer geometry to recover without patching each game. What
// we can do is resample the shading: inside a Gouraud-shaded surface (same
// colour, small intensity steps) each output pixel interpolates the intensity
// of its neighbours, while polygon edges, flat-shaded facets, textures and 2D
// art are copied pixel for pixel and stay sharp.
#include "jaguar.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace jag {

void Jaguar::setShadeTracking(bool on) {
    if (on && !shadeTag) {
        shadeTag = new uint32_t[DRAM_SIZE / 2]();
        fbInfo = new uint32_t[MAX_W * MAX_H]();
        memset(lineFrac, 0, sizeof lineFrac);
    } else if (!on && shadeTag) {
        delete[] shadeTag; shadeTag = nullptr;
        delete[] fbInfo; fbInfo = nullptr;
    }
}

namespace {
// 4x4 Bayer thresholds, in 1/256 of an output level.
const int kBayer[4][4] = {{0, 128, 32, 160}, {192, 64, 224, 96}, {48, 176, 16, 144}, {240, 112, 208, 80}};
}

void Jaguar::renderSmooth(int scale, uint32_t* out) const {
    const int ow = fbW * scale, oh = fbH * scale;
    if (!fbInfo) {   // tracking off: plain pixel copy
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) out[y * ow + x] = fb[(y / scale) * MAX_W + x / scale];
        return;
    }
    // Intensity in 1/256 steps (0..65535) of a frame pixel.
    auto inten = [&](uint32_t info) { return (int)((info >> 16) & 0xFF) * 256 + (int)(info & SH_FRAC); };
    for (int y = 0; y < oh; y++) {
        const int cy = y / scale;
        // Sample position in source pixels (centre-aligned), for interpolation.
        const int fy256 = ((2 * y + 1) * 256 / scale - 256) / 2;   // (y + 0.5) / scale - 0.5, in 1/256
        int y0 = fy256 >> 8; const int wy = fy256 & 255;
        for (int x = 0; x < ow; x++) {
            const int cx = x / scale;
            const uint32_t ci = fbInfo[cy * MAX_W + cx];
            if (!(ci & SH_SMOOTH)) { out[y * ow + x] = fb[cy * MAX_W + cx]; continue; }
            const int chroma = (int)(ci >> 24);
            const int ic = inten(ci);
            int I = ic;
            if (scale > 1) {
                const int fx256 = ((2 * x + 1) * 256 / scale - 256) / 2;
                const int x0 = fx256 >> 8, wx = fx256 & 255;
                // Neighbours that belong to the same smoothly shaded surface take
                // part; anything else stands in with the centre pixel's value.
                auto sample = [&](int sx, int sy) {
                    if (sx < 0 || sy < 0 || sx >= fbW || sy >= fbH) return ic;
                    uint32_t ni = fbInfo[sy * MAX_W + sx];
                    if (!(ni & SH_SMOOTH) || (int)(ni >> 24) != chroma) return ic;
                    int v = inten(ni);
                    return std::abs(v - ic) <= 4 * 256 ? v : ic;
                };
                int a = sample(x0, y0), b = sample(x0 + 1, y0), c = sample(x0, y0 + 1), d = sample(x0 + 1, y0 + 1);
                int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
                I = (int)(((int64_t)top * (256 - wy) + (int64_t)bot * wy) >> 16);
            }
            // Colour at full intensity, scaled by the high-precision intensity,
            // then dithered down to 8 bits per channel.
            const uint32_t full = cryTable[(chroma << 8) | 255];
            const int t = kBayer[y & 3][x & 3];
            uint32_t px = 0xFF000000u;
            for (int k = 0; k < 3; k++) {
                int comp = (int)((full >> (8 * k)) & 0xFF);            // 0..255 at Y = 255
                int v = (int)(((int64_t)comp * I) / 255);              // 1/256 output steps
                int o = (v + t) >> 8;
                px |= (uint32_t)std::min(255, std::max(0, o)) << (8 * k);
            }
            out[y * ow + x] = px;
        }
    }
}

} // namespace jag

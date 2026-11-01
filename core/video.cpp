// JagDroid - TOM object processor and video output.
#include "jaguar.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace jag {

// CRY colour: the colour byte is a point on a 16x16 square (high nibble = U,
// low nibble = V); the low byte is intensity. Per the hardware manual the
// square is the RGB cube's outer surface seen down the grey axis (a hexagon
// with white at the centre) stretched onto a square, with green at the middle
// of one edge. Orientation calibrated against real-hardware references:
// $00 = blue (Cybermorph's sky), $F0 = red (BIOS logo), $70 = magenta (stars),
// $C9/$DA = gold (title text), $56/$66 = pale lavender (title logo).
//
//            V=0              V=7.5             V=15
//   U = 0    B (0,0) -------------------------- C (0,15)
//            |                                     |
//   U = 7.5  M (7.5,0)        white           G (7.5,15)
//            |                                     |
//   U = 15   R (15,0) ------------------------- Y (15,15)
//
// Each chroma point is a blend from white (centre) to the fully saturated
// colour on the edge, so one channel is always at full strength, and the
// intensity byte scales the result. This is constructed from that geometry,
// not copied from the hardware's ROM tables, so it is close but not bit-exact.
void Jaguar::buildCryTable() {
    struct Anchor { double t, r, g, b; };
    // Perimeter position t: U=0 edge (V 0->15), V=15 edge (U 0->15),
    // U=15 edge (V 15->0), V=0 edge (U 15->0).
    static const Anchor ring[] = {
        {0.0, 0, 0, 1},   // B  (0,0)
        {15.0, 0, 1, 1},  // C  (0,15)
        {22.5, 0, 1, 0},  // G  (7.5,15)
        {30.0, 1, 1, 0},  // Y  (15,15)
        {45.0, 1, 0, 0},  // R  (15,0)
        {52.5, 1, 0, 1},  // M  (7.5,0)
        {60.0, 0, 0, 1},  // back to B
    };
    auto edgeColour = [&](double t, double& r, double& g, double& b) {
        for (int i = 0; i < 6; i++) {
            if (t >= ring[i].t && t <= ring[i + 1].t) {
                double f = (t - ring[i].t) / (ring[i + 1].t - ring[i].t);
                r = ring[i].r + (ring[i + 1].r - ring[i].r) * f;
                g = ring[i].g + (ring[i + 1].g - ring[i].g) * f;
                b = ring[i].b + (ring[i + 1].b - ring[i].b) * f;
                return;
            }
        }
        r = g = b = 1;
    };

    for (int c = 0; c < 256; c++) {
        double u = c >> 4, v = c & 15;
        double du = u - 7.5, dv = v - 7.5;
        double m = std::max(std::fabs(du), std::fabs(dv));
        double r = 1, g = 1, b = 1;
        if (m > 0) {
            // Project from the centre onto the square's edge
            double qu = 7.5 + du * 7.5 / m, qv = 7.5 + dv * 7.5 / m;
            double t;
            if (qu <= 1e-9) t = qv;                 // U = 0 edge, V 0->15
            else if (qv >= 15 - 1e-9) t = 15 + qu;  // V = 15 edge, U 0->15
            else if (qu >= 15 - 1e-9) t = 30 + (15 - qv);  // U = 15 edge, V 15->0
            else t = 45 + (15 - qu);                // V = 0 edge, U 15->0
            double er, eg, eb;
            edgeColour(t, er, eg, eb);
            double s = m / 7.5;                     // 0 at centre, 1 on the edge
            r = 1 + (er - 1) * s; g = 1 + (eg - 1) * s; b = 1 + (eb - 1) * s;
        }
        for (int y = 0; y < 256; y++) {
            uint32_t R = (uint32_t)lrint(r * y), G = (uint32_t)lrint(g * y), B = (uint32_t)lrint(b * y);
            cryTable[(c << 8) | y] = 0xFF000000u | (B << 16) | (G << 8) | R;
        }
    }
}

static inline uint32_t rgb16ToRgba(uint16_t p) {
    uint32_t r = (p >> 11) & 0x1F, b = (p >> 6) & 0x1F, g = p & 0x3F;
    r = (r << 3) | (r >> 2); b = (b << 3) | (b >> 2); g = (g << 2) | (g >> 4);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint64_t readPhrase(Jaguar* j, uint32_t a) {
    return (uint64_t)j->read32(a) << 32 | j->read32(a + 4);
}
static inline void writePhrase(Jaguar* j, uint32_t a, uint64_t v) {
    j->write32(a, (uint32_t)(v >> 32));
    j->write32(a + 4, (uint32_t)v);
}

// Draw one line of a bitmap object into the line buffer.
static const bool g_skipRmw = getenv("JAG_SKIPRMW") != nullptr;   // diagnostics only
static void drawBitmapLine(Jaguar* j, uint32_t data, uint64_t p1, int hscale /* 0x20 = 1.0 */) {
    int xpos = (int)(p1 & 0xFFF); if (xpos & 0x800) xpos -= 0x1000;
    int depth = (p1 >> 12) & 7;
    // Pitch is the phrase step within a line. 0 means "reuse the same phrase"
    // across the whole width: AvP and Cybermorph use 1bpp full-screen fade
    // layers like this, and reading on through memory instead added
    // vertical stripes of garbage to the picture.
    int pitch = (p1 >> 15) & 7;
    int iwidth = (p1 >> 28) & 0x3FF;
    int index = (p1 >> 38) & 0x7F;
    bool reflect = (p1 >> 45) & 1;
    bool rmw = (p1 >> 46) & 1;
    if (rmw && g_skipRmw) return;
    bool trans = (p1 >> 47) & 1;
    int firstpix = (p1 >> 49) & 0x3F;
    if (depth > 5) depth = 5;
    const int bpp = 1 << depth;
    // FIRSTPIX counts bits into the first phrase, so in pixels it is firstpix / bpp
    // (Wolfenstein 3D clips its view strips with it; treating it as a pixel count
    // cut 32 pixels off each strip and left black bars beside doors).
    firstpix /= bpp;
    const int ppp = 64 / bpp;                   // pixels per phrase
    const int total = iwidth * ppp;
    if (hscale <= 0) return;

    const uint8_t* clut = &j->tom[0x400];
    int outX = xpos * 32;                       // 27.5 fixed point position
    int step = reflect ? -hscale : hscale;
    uint64_t phrase = 0; int curPhrase = -1;

    for (int i = firstpix; i < total; i++) {
        int ph = i / ppp;
        if (ph != curPhrase) { phrase = readPhrase(j, data + (uint32_t)ph * pitch * 8); curPhrase = ph; }
        int shift = 64 - bpp * ((i % ppp) + 1);
        uint32_t pix = (uint32_t)((phrase >> shift) & ((bpp == 64) ? ~0ull : ((1ull << bpp) - 1)));

        int x0 = outX >> 5;
        outX += step;
        int x1 = outX >> 5;
        if (reflect) std::swap(x0, x1);
        if (x0 == x1) continue;

        if (trans && pix == 0) continue;
        if (bpp == 32) {
            for (int x = x0; x < x1; x++) if (x >= 0 && x < 720) { j->lineBuf32[x] = pix; j->lineFrac[x] = 0; }
            continue;
        }
        uint16_t val, info = 0;
        if (bpp <= 8) {
            uint32_t ci = (bpp == 8) ? pix : ((uint32_t)index << 1) + pix;
            ci &= 0xFF;
            val = (uint16_t)(clut[ci * 2] << 8 | clut[ci * 2 + 1]);
        } else {
            val = (uint16_t)pix;
            if (j->shadeTag) info = j->shadeInfo(data + (uint32_t)ph * pitch * 8 + (uint32_t)(i % ppp) * 2);
        }
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= 720) continue;
            if (!rmw) j->lineFrac[x] = info;
            if (rmw) {
                // Read-modify-write: each CRY colour nibble gets the object's
                // nibble added as a signed 4-bit offset, and intensity gets a
                // signed 8-bit add, all saturating. Calibrated on AvP: the
                // Alien HUD (offsets $DE/$EE) tints the tan corridor purple,
                // while Cybermorph's fade layers (offset $00) leave colour alone.
                uint16_t cur = j->lineBuf[x];
                auto sn = [](int n) { return n >= 8 ? n - 16 : n; };
                int u = ((cur >> 12) & 15) + sn((val >> 12) & 15);
                int v = ((cur >> 8) & 15) + sn((val >> 8) & 15);
                int y = (cur & 0xFF) + (int8_t)(val & 0xFF);
                u = u < 0 ? 0 : (u > 15 ? 15 : u);
                v = v < 0 ? 0 : (v > 15 ? 15 : v);
                y = y < 0 ? 0 : (y > 255 ? 255 : y);
                j->lineBuf[x] = (uint16_t)((u << 12) | (v << 8) | y);
            } else {
                j->lineBuf[x] = val;
            }
        }
    }
}

void Jaguar::opRenderLine(int vc) {
    uint32_t olp = ((uint32_t)tomReg16(0x22) << 16 | tomReg16(0x20)) & 0xFFFFF8;
    uint32_t addr = olp;
    for (int guard = 0; guard < 4096; guard++) {
        uint64_t p0 = readPhrase(this, addr);
        int type = (int)(p0 & 7);
        switch (type) {
        case 0:  // bitmap
        case 1: { // scaled bitmap
            int ypos = (int)((p0 >> 3) & 0x7FF);
            int height = (int)((p0 >> 14) & 0x3FF);
            uint32_t link = (uint32_t)((p0 >> 24) & 0x7FFFF) << 3;
            uint32_t data = (uint32_t)((p0 >> 43) & 0x1FFFFF) << 3;
            uint64_t p1 = readPhrase(this, addr + 8);
            int dwidth = (int)((p1 >> 18) & 0x3FF);
            if (verbose && getenv("JAG_OPDUMPVC") && vc == atoi(getenv("JAG_OPDUMPVC")) && getenv("JAG_OPDUMPF") && (long)frameCount == atol(getenv("JAG_OPDUMPF")))
                fprintf(stderr, "[op vc=%d] %06X type=%d y=%d h=%d data=%06X x=%d depth=%d iw=%d dw=%d pitch=%d first=%d\n", vc, addr, type, ypos, height, data,
                        (int)(p1 & 0xFFF), (int)((p1 >> 12) & 7), (int)((p1 >> 28) & 0x3FF), dwidth, (int)((p1 >> 15) & 7), (int)((p1 >> 49) & 0x3F));
            if (vc >= ypos && height > 0) {
                if (type == 0) {
                    drawBitmapLine(this, data, p1, 0x20);
                    data += (uint32_t)dwidth * 8;
                    height--;
                } else {
                    uint64_t p2 = readPhrase(this, addr + 16);
                    int hs = (int)(p2 & 0xFF), vs = (int)((p2 >> 8) & 0xFF);
                    int rem = (int)((p2 >> 16) & 0xFF);
                    drawBitmapLine(this, data, p1, hs);
                    rem -= 0x20;
                    while (rem < 0 && height > 0 && vs > 0) {
                        rem += vs; data += (uint32_t)dwidth * 8; height--;
                    }
                    if (vs == 0) height = 0;
                    if (rem < 0) rem = 0;
                    p2 = (p2 & ~(0xFFull << 16)) | ((uint64_t)(rem & 0xFF) << 16);
                    writePhrase(this, addr + 16, p2);
                }
                // The OP writes the updated height and data pointer back.
                p0 &= ~((0x3FFull << 14) | (0x1FFFFFull << 43));
                p0 |= ((uint64_t)height << 14) | ((uint64_t)(data >> 3) << 43);
                writePhrase(this, addr, p0);
            }
            addr = link;
            break;
        }
        case 2:  // GPU object: interrupt the GPU, continue with the next phrase
            gpu.raiseInt(3);
            addr += 8;
            break;
        case 3: { // branch
            int ypos = (int)((p0 >> 3) & 0x7FF);
            int cc = (int)((p0 >> 14) & 7);
            uint32_t link = (uint32_t)((p0 >> 24) & 0x7FFFF) << 3;
            bool take = false;
            switch (cc) {
                case 0: take = (ypos == vc) || (ypos == 0x7FF); break;
                case 1: take = ypos > vc; break;
                case 2: take = ypos < vc; break;
                case 3: take = (tomReg16(0x26) & 1) != 0; break;
                default: take = false; break;
            }
            addr = take ? link : addr + 8;
            break;
        }
        case 4:  // stop
            if (p0 & 8) { tomRaise(2); }
            return;
        default:
            return;
        }
        if (addr == 0) return;
    }
}

void Jaguar::outputLine(int vc) {
    uint16_t vmode = tomReg16(0x28);
    int vdb = tomReg16(0x46) & ~1;
    int row = (vc - vdb) / 2;
    if (row < 0 || row >= MAX_H) return;
    uint32_t* out = fb + row * MAX_W;
    const int mode = (vmode >> 1) & 3;
    const bool varmod = (vmode >> 8) & 1;
    const bool enabled = vmode & 1;

    const int linePw = ((vmode >> 9) & 7) + 1;
    if (linePw < minPwSeen) minPwSeen = linePw;
    // Source step in 1/256 pixels: wider pixels on this line than the frame grid get repeated.
    const int step = linePw > basePw ? (basePw * 256) / linePw : 256;

    if (rawOut) memcpy(rawOut + row * MAX_W, lineBuf, sizeof(uint16_t) * fbW);
    if (fbInfo) {
        uint32_t* inf = fbInfo + row * MAX_W;
        for (int x = 0; x < fbW; x++) {
            const int sx = (x * step) >> 8;
            uint16_t p = lineBuf[sx];
            bool cry = enabled && (varmod ? (mode != 1 && !(p & 1)) : mode == 0);
            inf[x] = cry ? ((uint32_t)p << 16 | SH_CRY | lineFrac[sx]) : 0;
        }
    }
    for (int x = 0; x < fbW; x++) {
        if (!enabled) { out[x] = 0xFF000000u; continue; }
        const int sx = (x * step) >> 8;
        uint16_t p = lineBuf[sx];
        // Mixed mode: in the 16-bit modes each pixel's low bit picks RGB (1) or CRY (0).
        if (varmod && mode != 1) { out[x] = (p & 1) ? rgb16ToRgba(p) : cryTable[p]; continue; }
        switch (mode) {
            case 0: out[x] = cryTable[p]; break;                                   // CRY16
            case 1: {                                                              // RGB24
                uint32_t v = lineBuf32[sx];  // format: G R x B per Jaguar RGB24
                uint32_t r = (v >> 16) & 0xFF, g = (v >> 24) & 0xFF, b = v & 0xFF;
                out[x] = 0xFF000000u | (b << 16) | (g << 8) | r; break;
            }
            case 2: out[x] = rgb16ToRgba(p); break;                                // DIRECT16 (shown as RGB)
            default:
                if (varmod) out[x] = (p & 1) ? rgb16ToRgba(p) : cryTable[p];
                else out[x] = rgb16ToRgba(p);
                break;
        }
    }

    // Clear the line buffer for the next line if background fill is enabled.
    if (vmode & 0x80) {
        uint16_t bg = tomReg16(0x58);
        std::fill(lineBuf, lineBuf + 720, bg);
        std::fill(lineBuf32, lineBuf32 + 720, 0u);
        if (shadeTag) std::fill(lineFrac, lineFrac + 720, (uint16_t)0);
    }
}

} // namespace jag

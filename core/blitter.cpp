// JagDroid - TOM blitter.
// Runs each blit to completion when B_CMD is written. Covers pixel and phrase
// addressing, LFU logic ops, pattern fill, source/dest compare (transparency),
// bit-to-colour expansion, clipping, intensity add and basic Gouraud shading.
// Not yet implemented: Z-buffering and the exact phrase-mode alignment quirks.
#include "jaguar.h"
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <algorithm>
extern "C" {
#include "m68k.h"
}

namespace jag {

namespace {
enum : uint32_t {
    A1_BASE = 0x2200, A1_FLAGS = 0x2204, A1_CLIP = 0x2208, A1_PIXEL = 0x220C,
    A1_STEP = 0x2210, A1_FSTEP = 0x2214, A1_FPIXEL = 0x2218, A1_INC = 0x221C, A1_FINC = 0x2220,
    A2_BASE = 0x2224, A2_FLAGS = 0x2228, A2_MASK = 0x222C, A2_PIXEL = 0x2230, A2_STEP = 0x2234,
    B_CMD = 0x2238, B_COUNT = 0x223C, B_SRCD = 0x2240, B_DSTD = 0x2248, B_PATD = 0x2268,
    B_IINC = 0x2270, B_ZINC = 0x2274, B_I3 = 0x227C, B_Z3 = 0x228C, B_SRCZ1 = 0x2258,
};

struct Area {
    uint32_t base;
    int bpp, width, pitchMul, xadd, zoffs;
    bool yadd, xsign, ysign;
};

Area decodeArea(uint32_t base, uint32_t fl) {
    static const int pitchTab[4] = {1, 2, 4, 3};  // phrases between consecutive data phrases
    Area a;
    a.base = base & 0xFFFFF8;
    int ps = (fl >> 3) & 7; if (ps > 5) ps = 5;
    a.bpp = 1 << ps;
    int w = (fl >> 9) & 0x3F;
    a.width = ((4 | (w & 3)) << (w >> 2)) >> 2;
    if (a.width <= 0) a.width = 1;
    a.pitchMul = pitchTab[fl & 3];
    a.zoffs = (fl >> 6) & 7;   // Z data sits this many phrases after the pixel data
    a.xadd = (fl >> 16) & 3;
    a.yadd = (fl >> 18) & 1;
    a.xsign = (fl >> 19) & 1;
    a.ysign = (fl >> 20) & 1;
    return a;
}

inline uint32_t pixelAddrBits(const Area& a, int x, int y, uint32_t& bitInByte) {
    int64_t bits = ((int64_t)y * a.width + x) * a.bpp;
    if (bits < 0) bits = 0;
    int64_t phrase = bits >> 6, inPhrase = bits & 63;
    int64_t full = (int64_t)a.base + phrase * a.pitchMul * 8 + (inPhrase >> 3);
    bitInByte = (uint32_t)(inPhrase & 7);
    // A pointer that has run off the end of the 24-bit address space does not
    // wrap round into low memory (Tempest 2000's PAL screen effects do this).
    if (full > 0xFFFFFF) return 0xFFFFFFFFu;
    return (uint32_t)full;
}

uint32_t readPix(Jaguar* j, const Area& a, int x, int y) {
    uint32_t bit;
    uint32_t addr = pixelAddrBits(a, x, y, bit);
    if (addr == 0xFFFFFFFFu) return 0;
    switch (a.bpp) {
        case 1: case 2: case 4: {
            uint8_t b = j->read8(addr);
            int shift = 8 - a.bpp - (int)bit;
            return (b >> shift) & ((1u << a.bpp) - 1);
        }
        case 8: return j->read8(addr);
        case 16: return j->read16(addr);
        default: return j->read32(addr);
    }
}

void writePix(Jaguar* j, const Area& a, int x, int y, uint32_t v) {
    uint32_t bit;
    uint32_t addr = pixelAddrBits(a, x, y, bit);
    if (addr == 0xFFFFFFFFu) return;
    switch (a.bpp) {
        case 1: case 2: case 4: {
            uint8_t b = j->read8(addr);
            int shift = 8 - a.bpp - (int)bit;
            uint8_t mask = (uint8_t)(((1u << a.bpp) - 1) << shift);
            b = (uint8_t)((b & ~mask) | ((v << shift) & mask));
            j->write8(addr, b);
            break;
        }
        case 8: j->write8(addr, (uint8_t)v); break;
        case 16: j->write16(addr, (uint16_t)v); break;
        default: j->write32(addr, v); break;
    }
}

// Z values are 16 bits, interleaved with the pixel data: same pixel position,
// ZOFFS phrases further on.
inline uint32_t zAddr(const Area& a, int x, int y) {
    Area z = a; z.bpp = 16;
    uint32_t bit;
    uint32_t za = pixelAddrBits(z, x, y, bit);
    return za == 0xFFFFFFFFu ? za : za + (uint32_t)a.zoffs * 8;
}

inline uint32_t fromPhrase(uint64_t ph, int bpp, int x) {
    if (bpp >= 64) return (uint32_t)ph;
    int ppp = 64 / bpp;
    int idx = ((x % ppp) + ppp) % ppp;
    int shift = 64 - bpp * (idx + 1);
    uint64_t mask = (bpp == 32) ? 0xFFFFFFFFull : ((1ull << bpp) - 1);
    return (uint32_t)((ph >> shift) & mask);
}

inline uint32_t lfu(int f, uint32_t s, uint32_t d) {
    uint32_t r = 0;
    if (f & 1) r |= ~s & ~d;
    if (f & 2) r |= ~s & d;
    if (f & 4) r |= s & ~d;
    if (f & 8) r |= s & d;
    return r;
}
} // namespace

static inline uint32_t j_read16(Jaguar* j, uint32_t a) { return j->read16(a); }

void Jaguar::blitterGo() {
    blitCount++;
    int prevMaster = busMaster; busMaster = 3;
    auto R32 = [&](uint32_t o) { return tomReg32(o); };
    auto R64 = [&](uint32_t o) { return (uint64_t)tomReg32(o) << 32 | tomReg32(o + 4); };
    auto W32 = [&](uint32_t o, uint32_t v) {
        tom[o] = v >> 24; tom[o+1] = (v >> 16) & 0xFF; tom[o+2] = (v >> 8) & 0xFF; tom[o+3] = v & 0xFF;
    };

    const uint32_t cmd = R32(B_CMD);
    if (verbose && getenv("JAG_GOURLOG") && (cmd & (1u << 12)) && (long)frameCount >= atol(getenv("JAG_GOURLOG")))
        fprintf(stderr, "[gour] x=%d cnt=%d I3=%08X I2=%08X I1=%08X I0=%08X IINC=%08X Z3=%08X Z2=%08X Z1=%08X Z0=%08X ZINC=%08X\n",
                (int16_t)(R32(A1_PIXEL) & 0xFFFF), (int)(R32(B_COUNT) & 0xFFFF), R32(B_I3), R32(B_I3 + 4), R32(B_I3 + 8), R32(B_I3 + 12),
                R32(B_IINC), R32(B_Z3), R32(B_Z3 + 4), R32(B_Z3 + 8), R32(B_Z3 + 12), R32(B_ZINC));
    if (verbose && getenv("JAG_BLITLOG") && (getenv("JAG_BLITFRAME") ? (long)frameCount == atol(getenv("JAG_BLITFRAME")) : (getenv("JAG_BLITFROM") ? (long)frameCount >= atol(getenv("JAG_BLITFROM")) : (blitCount <= 400 || (frameCount >= 1790 && blitCount % 7 == 0)))))
        fprintf(stderr, "[blit %llu] f%llu by=%s VC=%d cmd=%08X A1 base=%06X fl=%08X pix=%08X step=%08X fstep=%08X inc=%08X | A2 base=%06X fl=%08X pix=%08X step=%08X | cnt=%08X patd=%08X%08X srcd=%08X%08X iinc=%08X 68kPC=%06X gpuPC=%06X\n",
                (unsigned long long)blitCount, (unsigned long long)frameCount,
                prevMaster == 0 ? "68k" : (prevMaster == 1 ? "GPU" : (prevMaster == 2 ? "DSP" : "other")), halfLine, cmd, R32(A1_BASE), R32(A1_FLAGS), R32(A1_PIXEL), R32(A1_STEP), R32(A1_FSTEP), R32(A1_INC),
                R32(A2_BASE), R32(A2_FLAGS), R32(A2_PIXEL), R32(A2_STEP), R32(B_COUNT), R32(B_PATD), R32(B_PATD + 4), R32(B_SRCD), R32(B_SRCD + 4), R32(B_IINC), m68k_get_reg(nullptr, M68K_REG_PPC), gpu.pc);
    if (verbose && getenv("JAG_BLITDUMP") && (long)frameCount == atol(getenv("JAG_BLITDUMP")) && cmd == 0x01800F01) {
        static bool done = false;
        if (!done) { done = true; if (FILE* f = fopen("/tmp/claude-0/dbg/gpuram.bin", "wb")) { fwrite(gpu.ram, 1, gpu.ramSize, f); fclose(f); }
            fprintf(stderr, "[blitdump] GPU RAM saved, gpu pc=%06X\n", gpu.pc); }
    }
    // SRCENX (bit 2) also reads the source; Hover Strike's text blits set only it.
    const bool srcen = (cmd & (1u << 0)) || ((cmd & (1u << 2)) && !getenv("JAG_X_NOSRCENX")), dsten = cmd & (1u << 3);
    const bool srcenx = cmd & (1u << 2);
    const bool clipA1 = cmd & (1u << 6);
    const bool upda1f = cmd & (1u << 8), upda1 = cmd & (1u << 9), upda2 = cmd & (1u << 10);
    const bool dsta2 = cmd & (1u << 11);
    const bool gourd = cmd & (1u << 12);
    const bool patdsel = cmd & (1u << 16), adddsel = cmd & (1u << 17);
    const int lfuFunc = (cmd >> 21) & 0xF;
    const bool bcompen = cmd & (1u << 26), dcompen = cmd & (1u << 27);
    const bool bkgwren = cmd & (1u << 28);
    const bool srcenz = cmd & (1u << 1);
    const bool dstenz = cmd & (1u << 4), dstwrz = cmd & (1u << 5);
    const bool zbuff = cmd & (1u << 13);
    const int zmode = (cmd >> 18) & 7;   // inhibit when source Z is: bit0 less, bit1 equal, bit2 greater
    const bool srcshade = cmd & (1u << 30);
    const bool cmpdst = cmd & (1u << 25);

    Area a1 = decodeArea(R32(A1_BASE), R32(A1_FLAGS));
    Area a2 = decodeArea(R32(A2_BASE), R32(A2_FLAGS));
    const bool a2mask = (R32(A2_FLAGS) >> 15) & 1;
    const uint32_t mask = R32(A2_MASK);

    // A1 has 16.16 position; A2 integer
    uint32_t p1 = R32(A1_PIXEL), f1 = R32(A1_FPIXEL);
    int64_t a1x = ((int64_t)(int16_t)(p1 & 0xFFFF) << 16) | (f1 & 0xFFFF);
    int64_t a1y = ((int64_t)(int16_t)(p1 >> 16) << 16) | (f1 >> 16);
    uint32_t p2 = R32(A2_PIXEL);
    int a2x = (int16_t)(p2 & 0xFFFF), a2y = (int16_t)(p2 >> 16);

    uint32_t s1 = R32(A1_STEP), fs1 = R32(A1_FSTEP);
    int64_t step1x = ((int64_t)(int16_t)(s1 & 0xFFFF) << 16) | (upda1f ? (fs1 & 0xFFFF) : 0);
    int64_t step1y = ((int64_t)(int16_t)(s1 >> 16) << 16) | (upda1f ? (fs1 >> 16) : 0);
    uint32_t s2 = R32(A2_STEP);
    int step2x = (int16_t)(s2 & 0xFFFF), step2y = (int16_t)(s2 >> 16);
    uint32_t inc = R32(A1_INC), finc = R32(A1_FINC);
    int64_t inc1x = ((int64_t)(int16_t)(inc & 0xFFFF) << 16) | (finc & 0xFFFF);
    int64_t inc1y = ((int64_t)(int16_t)(inc >> 16) << 16) | (finc >> 16);

    uint32_t clip = R32(A1_CLIP);
    int clipW = clip & 0x7FFF, clipH = (clip >> 16) & 0x7FFF;

    uint32_t count = R32(B_COUNT);
    int inner = count & 0xFFFF, outer = count >> 16;
    // The inner counter stops when it reaches zero or wraps negative, so a
    // count of 0 wraps on the first step: one write per row, not 65536.
    // (Worms blits zero-width characters; treating 0 as 65536 sprayed six
    // 64K-pixel rows over its title screen.)
    if (inner == 0) inner = 1;
    if (outer == 0) outer = 1;

    const uint64_t patd = R64(B_PATD), srcd = R64(B_SRCD), dstd = R64(B_DSTD);
    const int32_t iinc = (int32_t)R32(B_IINC);
    // Gouraud intensities live in the pattern and source data registers: the
    // B_I0..B_I3 addresses are write aliases. Lane p of a phrase (0 = leftmost)
    // takes its integer intensity from PATD byte 2p+1 (beside its colour byte)
    // and its 16-bit fraction from SRCD bytes 2p..2p+1. Games that rewrite PATD
    // therefore restart the shading (Iron Soldier's menu gradient; keeping
    // separate registers let it creep brighter every frame and wrap).
    // All four lanes step by IINC once per phrase; pixel mode uses the
    // rightmost lane alone, stepping per pixel.
    auto laneInten = [&](int p) {
        return (uint32_t)tom[B_PATD + 1 + 2 * p] << 16 | (uint32_t)tom[B_SRCD + 2 * p] << 8 | tom[B_SRCD + 2 * p + 1];
    };
    uint32_t iph[4] = {laneInten(0), laneInten(1), laneInten(2), laneInten(3)};
    uint32_t inten = iph[3];
    // Intensities saturate at 0 and $FFFFFF rather than wrapping.
    const int32_t iinc24 = (int32_t)((uint32_t)iinc << 8) >> 8;
    auto stepI = [&](uint32_t& v) {
        int32_t n = (int32_t)(v & 0xFFFFFF) + iinc24;
        v = (uint32_t)(n < 0 ? 0 : (n > 0xFFFFFF ? 0xFFFFFF : n));
    };
    long lastPhrase = LONG_MIN;
    // Z interpolation mirrors the intensity scheme: Z3..Z0 per phrase pixel
    // stepping by ZINC per phrase, or Z3 stepping by ZINC per pixel.
    const int32_t zinc = (int32_t)R32(B_ZINC);
    uint32_t zph[4] = {R32(B_Z3 + 12), R32(B_Z3 + 8), R32(B_Z3 + 4), R32(B_Z3)};   // Z0..Z3
    uint32_t zpix = zph[3];   // pixel mode uses Z3
    long lastZPhrase = LONG_MIN;
    const bool useZ = zbuff || dstwrz;

    Area& dstA = dsta2 ? a2 : a1;
    Area& srcA = dsta2 ? a1 : a2;

    int64_t a1EndPhrase = 0;
    int a2EndPhrase = 0;
    auto advance1 = [&]() {
        switch (a1.xadd) {
            case 0: case 1: a1x += a1.xsign ? -(1 << 16) : (1 << 16); break;
            case 2: break;
            case 3: a1x += inc1x; a1y += inc1y; break;
        }
        // In increment mode the fractional INC is A1's whole per-pixel step;
        // AvP texture-maps with YADD also set, and adding a full row on top
        // would make textures impossible to magnify.
        if (a1.yadd && a1.xadd != 3) a1y += a1.ysign ? -(1 << 16) : (1 << 16);
    };
    auto advance2 = [&]() {
        if (a2.xadd == 0 || a2.xadd == 1) a2x += a2.xsign ? -1 : 1;
        // Per-pixel Y step: AvP draws its walls as vertical columns this way.
        if (a2.yadd) a2y += a2.ysign ? -1 : 1;
    };

    // In phrase mode (XADD = 0) the hardware moves whole 64-bit phrases, so at
    // the end of each row a pointer sits on the phrase boundary after the last
    // phrase touched, not one pixel past the last pixel. Games rely on this
    // when they program the step that takes them to the next row.
    auto floorMod = [](int v, int m) { return ((v % m) + m) % m; };
    auto phraseEnd = [&](int start, int ppp, int nphr, bool neg) {
        int base = start - floorMod(start, ppp);
        return neg ? base - (nphr - 1) * ppp - 1 : base + nphr * ppp;
    };

    // Data taken from the 64-bit registers (pattern, source, destination):
    // phrase mode picks the field under the pixel's position in the phrase;
    // pixel mode always uses the low field of the long at the register's own
    // address ($F02268 for the pattern). Kasumi Ninja and Iron Soldier write
    // only that long before pixel-mode fills and lines.
    const bool dstPhrase = dstA.xadd == 0;
    const uint32_t dmask = dstA.bpp >= 32 ? 0xFFFFFFFFu : ((1u << dstA.bpp) - 1);
    auto regData = [&](uint64_t ph, int x) {
        static bool oldReg = getenv("JAG_X_OLDREG") != nullptr;
        return (dstPhrase || oldReg) ? fromPhrase(ph, dstA.bpp, x) : (uint32_t)(ph >> 32) & dmask;
    };

    long long work = 0;
    for (int o = 0; o < outer; o++) {
        const int64_t a1x0 = a1x;
        const int a2x0 = a2x;
        {
            int dStart = dsta2 ? a2x0 : (int)(a1x0 >> 16);
            int dppp = 64 / dstA.bpp;
            bool dneg = dstA.xsign;
            int lead = dneg ? (dppp - 1 - floorMod(dStart, dppp)) : floorMod(dStart, dppp);
            int nphr = (lead + inner + dppp - 1) / dppp;
            // Pointer positions after this row in phrase mode
            if (a1.xadd == 0) {
                int p1 = 64 / a1.bpp;
                int startPix = dsta2 ? (int)(a1x0 >> 16) : dStart;
                a1EndPhrase = (int64_t)phraseEnd(startPix, p1, nphr, a1.xsign) << 16;
            }
            if (a2.xadd == 0) {
                int p2 = 64 / a2.bpp;
                a2EndPhrase = phraseEnd(a2x0, p2, nphr, a2.xsign);
            }
        }
        // Bit expansion reads the source at its own depth and steps through
        // each source pixel's bits MSB first, one destination pixel per bit;
        // a row narrower than a source pixel uses its rightmost bits.
        const int srcBits = std::max(1, srcA.bpp >= 32 ? 32 : srcA.bpp);
        int bitPos = (bcompen && srcenx && inner < srcBits) ? srcBits - inner : 0;
        for (int i = 0; i < inner; i++) {
            int x1 = (int)(a1x >> 16), y1 = (int)(a1y >> 16);
            int x2 = a2x, y2 = a2y;
            if (a2mask) { x2 &= (int)(mask & 0xFFFF); y2 &= (int)(mask >> 16); }
            int dx = dsta2 ? x2 : x1, dy = dsta2 ? y2 : y1;
            int sx = dsta2 ? x1 : x2, sy = dsta2 ? y1 : y2;

            bool inhibit = false;
            // Clipping follows the A1 pointer even when A1 is the source (DSTA2):
            // samples taken outside the A1 window are not written (Cybermorph's scaled text).
            if (clipA1 && (x1 < 0 || y1 < 0 || x1 >= clipW || y1 >= clipH)) inhibit = true;

            uint32_t src = srcen ? readPix(this, srcA, sx, sy) : regData(srcd, dx);
            const uint32_t rawSrc = src;   // transparency (DCOMPEN) tests the unshaded source
            if (srcshade && !bcompen) {
                // SRCSHADE adjusts each 16-bit CRY source pixel by IINC: bits 23-16
                // (with the 16-bit fraction) are a signed intensity change, and the
                // top byte shifts the colour, each nibble a signed 4-bit amount; all
                // saturate. Battle Morph darkens its menu panels in place this way
                // (IINC $00D8xxxx = -40) and colours its grey font: $77 gives
                // yellow letters, $78 the green tick and $07 the red arrow.
                auto shade = [&](uint32_t p16) {
                    int32_t inc24 = (int32_t)((uint32_t)iinc << 8) >> 8;
                    int32_t y = ((int32_t)((p16 & 0xFF) << 16) + inc24) >> 16;
                    y = y < 0 ? 0 : (y > 255 ? 255 : y);
                    auto sn = [](int n) { return n >= 8 ? n - 16 : n; };
                    const int dc = (int)((uint32_t)iinc >> 24);
                    int c = (int)((p16 >> 12) & 15) + sn(dc >> 4);
                    int r = (int)((p16 >> 8) & 15) + sn(dc & 15);
                    c = c < 0 ? 0 : (c > 15 ? 15 : c);
                    r = r < 0 ? 0 : (r > 15 ? 15 : r);
                    return (uint32_t)(c << 12 | r << 8 | y);
                };
                if (srcA.bpp == 16) src = shade(src);
                else if (srcA.bpp == 32) src = shade(src >> 16) << 16 | shade(src & 0xFFFF);
            }
            uint32_t dst = dsten ? readPix(this, dstA, dx, dy) : regData(dstd, dx);

            uint32_t out;
            if (bcompen) {
                // Bit expansion: the source pixel is a mask bit.
                uint32_t bitv = (src >> ((~bitPos) & (srcBits - 1))) & 1;
                bitPos++;
                if (bitv) out = patdsel ? regData(patd, dx) : lfu(lfuFunc, src, dst);
                else if (bkgwren) out = regData(dstd, dx);
                else { inhibit = true; out = 0; }
            } else if (patdsel) {
                out = regData(patd, dx);
            } else if (adddsel) {
                // The adder works on 16-bit lanes whatever the pixel size: each
                // lane keeps the source's colour byte and adds the source's
                // signed intensity byte to the destination's, saturating.
                // (Ruiner Pinball clears a 32-bit area this way with $0081 lanes;
                // a plain 32-bit add left junk that later overwrote its stack.)
                auto lane = [](uint32_t s16, uint32_t d16) {
                    int y = (int)(d16 & 0xFF) + (int8_t)(s16 & 0xFF);
                    y = y < 0 ? 0 : (y > 255 ? 255 : y);
                    return (s16 & 0xFF00) | (uint32_t)y;
                };
                if (dstA.bpp == 16) out = lane(src, dst);
                else if (dstA.bpp == 32) out = lane(src >> 16, dst >> 16) << 16 | lane(src & 0xFFFF, dst & 0xFFFF);
                else out = src + dst;
            } else {
                out = lfu(lfuFunc, src, dst);
            }
            uint32_t ivUsed = 0;
            if (gourd && dstA.bpp == 16) {
                uint32_t iv;
                if (dstA.xadd == 0) {
                    long ph = (long)((dx >= 0) ? dx / 4 : (dx - 3) / 4);
                    if (lastPhrase != LONG_MIN && ph != lastPhrase)
                        for (auto& v : iph) stepI(v);
                    lastPhrase = ph;
                    iv = iph[((dx % 4) + 4) % 4];
                } else {
                    iv = inten;
                    stepI(inten);
                }
                out = (fromPhrase(patd, 16, dstA.xadd == 0 ? dx : 3) & 0xFF00) | ((iv >> 16) & 0xFF);
                ivUsed = iv;
            }
            if (dcompen) {
                // The data comparator matches against the pattern register: the
                // source pixel (or with CMPDST the destination) equal to PATD is
                // not written. Most games key on 0; Brutal Sports keys its mud
                // marks on $F0, which we drew as black boxes. Zero stays
                // transparent too, as games rely on it with a stale PATD.
                const uint32_t key = regData(patd, dx);
                if (cmpdst) { if (dst == key) inhibit = true; }
                else if (rawSrc == 0 || rawSrc == key) inhibit = true;
            }

            // Z buffer
            uint32_t srcZ = 0, zaddr = 0;
            if (useZ && dstA.bpp == 16) {
                if (srcenz) {
                    srcZ = j_read16(this, zAddr(srcA, sx, sy));
                } else if (dstA.xadd == 0) {
                    long ph = (long)((dx >= 0) ? dx / 4 : (dx - 3) / 4);
                    if (lastZPhrase != LONG_MIN && ph != lastZPhrase)
                        for (auto& v : zph) v += (uint32_t)zinc;
                    lastZPhrase = ph;
                    srcZ = zph[((dx % 4) + 4) % 4] >> 16;
                } else {
                    srcZ = zpix >> 16;
                    zpix += (uint32_t)zinc;
                }
                zaddr = zAddr(dstA, dx, dy);
                if (zbuff && dstenz && zaddr != 0xFFFFFFFFu) {
                    uint32_t dstZ = j_read16(this, zaddr);
                    if (((zmode & 1) && srcZ < dstZ) || ((zmode & 2) && srcZ == dstZ) ||
                        ((zmode & 4) && srcZ > dstZ))
                        inhibit = true;
                }
            }

            if (!inhibit) {
                uint32_t m = dstA.bpp >= 32 ? 0xFFFFFFFFu : ((1u << dstA.bpp) - 1);
                writePix(this, dstA, dx, dy, out & m);
                if (shadeTag && dstA.bpp == 16) {   // display enhancement bookkeeping
                    uint32_t bit, da = pixelAddrBits(dstA, dx, dy, bit);
                    uint16_t info = 0;
                    if (gourd) info = (uint16_t)(SH_SHADED | (iinc ? SH_SMOOTH : 0) | ((ivUsed >> 8) & 0xFF));
                    else if (srcen && srcA.bpp == 16 && (out & 0xFFFF) == (src & 0xFFFF)) {   // a copy keeps it
                        uint32_t sa = pixelAddrBits(srcA, sx, sy, bit);
                        if (sa != 0xFFFFFFFFu) info = shadeInfo(sa);
                    }
                    if (da != 0xFFFFFFFFu) shadeNote(da, (uint16_t)out, info);
                }
                if (useZ && dstwrz && dstA.bpp == 16 && zaddr != 0xFFFFFFFFu) write16(zaddr, (uint16_t)srcZ);
            }
            if (bcompen && srcBits > 1) {
                // Only the destination moves every pixel; the source moves once its bits are used up.
                bool srcStep = (bitPos % srcBits) == 0;
                if (dsta2) { advance2(); if (srcStep) advance1(); }
                else { advance1(); if (srcStep) advance2(); }
            } else {
                advance1();
                advance2();
            }
            work++;
        }
        if (a1.xadd == 0) a1x = a1EndPhrase | (a1x0 & 0xFFFF);
        if (a2.xadd == 0) a2x = a2EndPhrase;
        if (upda1) { a1x += step1x; a1y += step1y; }
        if (upda2) { a2x += step2x; a2y += step2y; }
    }

    // Write back the pointers like the hardware does.
    W32(A1_PIXEL, (uint32_t)((((a1y >> 16) & 0xFFFF) << 16) | ((a1x >> 16) & 0xFFFF)));
    W32(A1_FPIXEL, (uint32_t)(((a1y & 0xFFFF) << 16) | (a1x & 0xFFFF)));
    W32(A2_PIXEL, (uint32_t)(((a2y & 0xFFFF) << 16) | (a2x & 0xFFFF)));
    if (useZ && !srcenz) {
        if (dstA.xadd == 0) {
            for (int k = 0; k < 4; k++) zph[k] += (uint32_t)zinc;
            for (int k = 0; k < 4; k++) W32(B_Z3 + 4 * k, zph[3 - k]);
        } else {
            W32(B_Z3, zpix);
        }
    }
    if (gourd) {
        if (dstA.xadd == 0) {
            for (int k = 0; k < 4; k++) stepI(iph[k]);   // the hardware leaves them one phrase on
        } else {
            iph[3] = inten;
        }
        // The accumulators are the PATD/SRCD lanes; leave the end values there
        // (and in the alias addresses, for anything that reads them back).
        for (int p = 0; p < 4; p++) {
            if (dstA.xadd != 0 && p != 3) continue;
            tom[B_PATD + 1 + 2 * p] = (uint8_t)(iph[p] >> 16);
            tom[B_SRCD + 2 * p] = (uint8_t)(iph[p] >> 8);
            tom[B_SRCD + 2 * p + 1] = (uint8_t)iph[p];
            W32(B_I3 + 4 * (3 - p), iph[p]);
        }
    }

    busMaster = prevMaster;
    // Time the blit by its memory accesses (about 1 system clock each in page
    // mode): the destination write plus any source, destination and Z reads and
    // Z writes. Phrase mode moves a whole 64-bit phrase per access.
    {
        int accesses = 1 + (srcen ? 1 : 0) + (dsten ? 1 : 0) + (useZ ? 1 + (dstwrz ? 1 : 0) : 0);
        double perPix = accesses * 1.0;
        if (dstA.xadd == 0 && dstA.bpp < 64) perPix /= (64 / std::max(1, dstA.bpp));
        blitBusy += work * perPix + outer * 4.0 + 8.0;
    }
    blitIntPending = true;   // GPU interrupt 4 fires when the work is done (tickTimers)
}

} // namespace jag

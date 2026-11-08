// JagDroid - Android glue: emulation thread, ANativeWindow video, AAudio sound,
// game saves (cartridge EEPROM), save states and scanline display modes.
// Video is copied straight into the window buffer and scaled by the display
// hardware, so no GL/Vulkan driver (stock or Turnip) is involved at all.
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <deque>
#include <string>
#include <thread>
#include <vector>
#include <time.h>
#include <unistd.h>
#include <map>
#include <cmath>
#include <dlfcn.h>
#include <android/hardware_buffer.h>
#include "jaguar.h"
#include "zipfile.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "JagDroid", __VA_ARGS__)

namespace {
jag::Jaguar* g_emu = nullptr;
std::mutex g_emuLock;              // held while a frame runs; save/load states take it too
std::string g_error;
std::thread g_thread;
std::atomic<bool> g_run{false};
std::atomic<bool> g_paused{false};
std::atomic<uint32_t> g_buttons{0};
std::atomic<float> g_fps{0.f};     // emulated frames per second, measured over ~0.5 s
std::atomic<int> g_scanlines{0};   // 0 off, 1 light, 2 strong, 3 LCD grid
std::atomic<bool> g_smoothShade{false};   // high-precision Gouraud shading
std::atomic<int> g_renderScale{1};        // 1 or 2
std::atomic<bool> g_cropOverscan{true};   // hide the border a TV would cut off
std::vector<uint32_t> g_hiFrame;          // enhanced picture when either is on
std::vector<uint32_t> g_stage;            // final SDR picture, before it goes to the window
std::atomic<bool> g_hdr{false};           // HDR output requested
std::atomic<float> g_hdrPeak{3.0f};       // brightest highlight, as a multiple of normal white
std::atomic<bool> g_hdrFailed{false};     // the screen or driver refused the HDR buffer format
bool g_winHdr = false;                    // format the window is currently set up for
bool g_pal = false;                // region chosen in the menu; applied when a game loads
std::string g_eepromPath;          // where this game's save chip lives

// CD games share one save chip (the Jaguar CD unit's Memory Track storage); it lives
// next to the per-game saves.
std::string memoryTrackPath() {
    size_t sl = g_eepromPath.find_last_of('/');
    return sl == std::string::npos ? std::string() : g_eepromPath.substr(0, sl + 1) + "jaguar-cd-memory-track.bin";
}

// The Memory Track cartridge (128 KB) is a separate save file, shared by all CD games too.
std::string nvmPath() {
    size_t sl = g_eepromPath.find_last_of('/');
    return sl == std::string::npos ? std::string() : g_eepromPath.substr(0, sl + 1) + "jaguar-cd-memory-track-cartridge.bin";
}

std::mutex g_winLock;
ANativeWindow* g_window = nullptr;
int g_winW = 0, g_winH = 0;

AAudioStream* g_stream = nullptr;
int g_streamRate = 0;

// Jaguar sound arrives at whatever rate the game programs (the BIOS jingle uses
// ~10.6 kHz). Handing that to the sound card unfiltered lets Android resample it
// crudely, which crackles. Resample here to a fixed 48 kHz with cubic
// interpolation and a gentle low-pass, as the console's analogue output stage did.
constexpr int OUT_RATE = 48000;
struct Resampler {
    float h[4][2] = {};          // last four input frames (oldest first)
    double pos = 0;              // fractional position between h[1] and h[2]
    float lp[2] = {0, 0};
    std::vector<int16_t> out;
    void reset() { std::memset(h, 0, sizeof h); pos = 0; lp[0] = lp[1] = 0; }
    void process(const int16_t* in, int frames, int inRate, double stepScale = 1.0) {
        out.clear();
        const double step = (double)inRate / OUT_RATE * stepScale;
        // low-pass corner just under the input Nyquist (capped for high rates)
        const float fc = std::min(0.42f * (float)inRate, 16000.0f);
        const float a = 1.0f - std::exp(-2.0f * 3.14159265f * fc / OUT_RATE);
        for (int i = 0; i < frames; i++) {
            for (int k = 0; k < 3; k++) { h[k][0] = h[k + 1][0]; h[k][1] = h[k + 1][1]; }
            h[3][0] = in[2 * i]; h[3][1] = in[2 * i + 1];
            while (pos < 1.0) {
                const float t = (float)pos;
                for (int c = 0; c < 2; c++) {
                    const float p0 = h[0][c], p1 = h[1][c], p2 = h[2][c], p3 = h[3][c];
                    float v = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
                    lp[c] += a * (v - lp[c]);
                    lp[c] += a * (v - lp[c]);   // two poles
                    float o = lp[c];
                    out.push_back((int16_t)std::max(-32768.0f, std::min(32767.0f, o)));
                }
                pos += step;
            }
            pos -= 1.0;
        }
    }
} g_resampler;

// The emulator thread pushes resampled frames into a lock-free ring; the sound
// card pulls them from its own callback. Writing straight to the stream made any
// late frame an audible gap, and the gaps were the pops and scratchiness. If the
// ring runs dry the output fades out instead of cutting, and the resampler
// speeds up or slows down by a fraction of a percent to hold the fill level.
constexpr int RING_FRAMES = 1 << 14;                  // ~340 ms
constexpr int TARGET_FILL = OUT_RATE * 70 / 1000;     // keep ~110 ms queued: rides out late frames without pitch changes
int16_t g_ring[RING_FRAMES * 2];
std::atomic<uint32_t> g_ringW{0}, g_ringR{0};
std::atomic<bool> g_ringPrimed{false};
float g_lastL = 0, g_lastR = 0;

inline uint32_t ringFill() { return g_ringW.load(std::memory_order_acquire) - g_ringR.load(std::memory_order_acquire); }

void ringPush(const int16_t* d, int frames) {
    uint32_t w = g_ringW.load(std::memory_order_relaxed);
    const uint32_t r = g_ringR.load(std::memory_order_acquire);
    int room = RING_FRAMES - (int)(w - r);
    if (frames > room) frames = room;   // overrun: drop the newest rather than glitch the oldest
    for (int i = 0; i < frames; i++) {
        g_ring[((w + i) & (RING_FRAMES - 1)) * 2] = d[2 * i];
        g_ring[((w + i) & (RING_FRAMES - 1)) * 2 + 1] = d[2 * i + 1];
    }
    g_ringW.store(w + frames, std::memory_order_release);
}

aaudio_data_callback_result_t audioCb(AAudioStream*, void*, void* data, int32_t n) {
    int16_t* o = (int16_t*)data;
    uint32_t r = g_ringR.load(std::memory_order_relaxed);
    const uint32_t w = g_ringW.load(std::memory_order_acquire);
    int avail = (int)(w - r);
    if (!g_ringPrimed.load(std::memory_order_relaxed)) {
        if (avail >= TARGET_FILL / 2) g_ringPrimed.store(true, std::memory_order_relaxed);
        else avail = 0;
    }
    int take = std::min(avail, (int)n);
    for (int i = 0; i < take; i++) {
        o[2 * i] = g_ring[((r + i) & (RING_FRAMES - 1)) * 2];
        o[2 * i + 1] = g_ring[((r + i) & (RING_FRAMES - 1)) * 2 + 1];
    }
    if (take > 0) { g_lastL = o[2 * (take - 1)]; g_lastR = o[2 * (take - 1) + 1]; }
    // Underrun: ease the last value to zero over what is missing.
    if (take < n) {
        const int miss = n - take;
        for (int i = 0; i < miss; i++) {
            const float k = 1.0f - (float)(i + 1) / (float)std::max(miss, 64);
            const float f = std::max(0.0f, k);
            o[2 * (take + i)] = (int16_t)(g_lastL * f);
            o[2 * (take + i) + 1] = (int16_t)(g_lastR * f);
        }
        g_lastL *= std::max(0.0f, 1.0f - (float)miss / 64.0f);
        g_lastR *= std::max(0.0f, 1.0f - (float)miss / 64.0f);
        if (take == 0) g_ringPrimed.store(false, std::memory_order_relaxed);   // refill before resuming
    }
    g_ringR.store(r + take, std::memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

std::atomic<bool> g_audioDead{false};   // the stream reported an error or stopped pulling data
void audioErr(AAudioStream*, void*, aaudio_result_t) { g_audioDead.store(true); }

void closeAudio() {
    if (g_stream) { AAudioStream_requestStop(g_stream); AAudioStream_close(g_stream); g_stream = nullptr; }
    g_streamRate = 0;
}

void openAudio(int rate) {
    closeAudio();
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, OUT_RATE);
    g_resampler.reset();
    g_ringW = 0; g_ringR = 0; g_ringPrimed = false; g_lastL = g_lastR = 0;
    AAudioStreamBuilder_setDataCallback(b, audioCb, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, audioErr, nullptr);
    g_audioDead = false;
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    if (AAudioStreamBuilder_openStream(b, &g_stream) == AAUDIO_OK) {
        int32_t burst = AAudioStream_getFramesPerBurst(g_stream);
        // The ring supplies the slack; keep the device buffer a few bursts deep.
        AAudioStream_setBufferSizeInFrames(g_stream, std::min(burst * 4, AAudioStream_getBufferCapacityInFrames(g_stream)));
        AAudioStream_requestStart(g_stream);
        g_streamRate = rate;
        LOG("audio %d Hz, burst %d", AAudioStream_getSampleRate(g_stream), burst);
    } else {
        g_stream = nullptr;
    }
    AAudioStreamBuilder_delete(b);
}

// Darken a pixel to `pct` percent (pixels are RGBA, R in the low byte).
// Each channel is scaled on its own: dividing red and blue packed together
// let blue's remainder spill into red and tinted scanlines pink.
inline uint32_t shade(uint32_t p, uint32_t pct) {
    const uint32_t k = pct * 256 / 100;   // 8.8 fixed-point factor
    uint32_t r = ((p & 0xFF) * k) >> 8, g = (((p >> 8) & 0xFF) * k) >> 8, b = (((p >> 16) & 0xFF) * k) >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Copy the frame into the window. With scanlines on, each emulated line becomes
// three window lines whose brightness falls off on the third, so the display's
// scaler draws dark gaps between lines like a CRT. The LCD grid also triples the
// width and darkens every third column.
// ---------------------------------------------------------------- HDR --
// The Jaguar's picture is ordinary SDR. In HDR mode it goes to the screen as
// linear extended-range light (scRGB, half floats), where 1.0 is normal white:
// everything up to mid-bright stays as it was, and the brightest part of the
// range is stretched up to the peak, so explosions, lasers and stars stand
// out. Scanlines dim the picture, so their lost brightness is put back too.
namespace hdr {
float toLinear[256];
bool tablesReady = false;
void buildTables() {
    for (int i = 0; i < 256; i++) {
        float c = i / 255.f;
        toLinear[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    tablesReady = true;
}
using SetDataSpace = int32_t (*)(ANativeWindow*, int32_t);
SetDataSpace setDataSpace() {   // API 28; looked up so the app still runs on 26-27
    static SetDataSpace fn = (SetDataSpace)dlsym(RTLD_DEFAULT, "ANativeWindow_setBuffersDataSpace");
    return fn;
}
const int32_t DATASPACE_SCRGB_LINEAR = 406913024;   // ADATASPACE_SCRGB_LINEAR
const int32_t DATASPACE_UNKNOWN = 0;

// Expand the top of the range: identity below the knee, then a curve that
// meets it smoothly and reaches `peak` at full white.
inline float expand(float l, float peak) {
    const float k = 0.55f;
    if (l <= k) return l;
    float t = (l - k) / (1.f - k);
    return l + (peak - 1.f) * t * t;
}

void convertRow(const uint32_t* s, __fp16* d, int n, float peak, float gain) {
    for (int x = 0; x < n; x++) {
        uint32_t p = s[x];
        float r = toLinear[p & 0xFF], g = toLinear[(p >> 8) & 0xFF], b = toLinear[(p >> 16) & 0xFF];
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float f = gain * (l > 1e-5f ? expand(l, peak) / l : 1.f);
        d[4 * x] = (__fp16)(r * f); d[4 * x + 1] = (__fp16)(g * f); d[4 * x + 2] = (__fp16)(b * f); d[4 * x + 3] = (__fp16)1.f;
    }
}
} // namespace hdr

void present() {
    std::lock_guard<std::mutex> lk(g_winLock);
    if (!g_window) return;
    const int mode = g_scanlines.load();
    const int scale = g_renderScale.load() == 2 ? 2 : 1;
    const bool enhanced = g_emu->shadeTracking();
    int w = g_emu->frameWidth() * scale, h = g_emu->frameHeight() * scale;
    // Source picture: the emulated frame, or the enhanced one built from it.
    const uint32_t* src = g_emu->framebuffer();
    int srcStride = jag::Jaguar::MAX_W;
    if (enhanced || scale > 1) {
        g_hiFrame.resize((size_t)w * h);
        g_emu->renderSmooth(scale, g_hiFrame.data());
        src = g_hiFrame.data(); srcStride = w;
    }
    // Overscan: games leave junk in the border a TV hides (Super Burnout's
    // bottom rows, Troy Aikman's players walking off the field). Trim about
    // 5% each side and 3% top and bottom, in whole emulated pixels.
    if (g_cropOverscan.load()) {
        const int cx = (g_emu->frameWidth() * 5 / 100) * scale, cy = (g_emu->frameHeight() * 3 / 100) * scale;
        if (w > 2 * cx + 64 && h > 2 * cy + 64) { src += (size_t)cy * srcStride + cx; w -= 2 * cx; h -= 2 * cy; }
    }
    // At 1x each emulated line becomes 3 output rows for scanlines; at 2x the
    // picture already has 2 rows per line, so the second one is the dark row.
    const int sx = (mode == 3 && scale == 1) ? 3 : 1, sy = (mode == 0 || scale == 2) ? 1 : 3;
    const int ww = w * sx, wh = h * sy;
    g_stage.resize((size_t)ww * wh);
    uint32_t* dst = g_stage.data();
    const int stride = ww;
    float lineAvg = 1.f;   // average brightness the scanlines leave, for HDR to restore
    if (mode == 0) {
        for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * stride, src + (size_t)y * srcStride, w * 4);
    } else if (scale == 2) {
        static const uint32_t darkPct[4] = {100, 70, 40, 55};
        lineAvg = (100.f + darkPct[mode]) / 200.f;
        if (mode == 3) lineAvg = (1.f + 0.6f + 0.55f + 0.35f) / 4.f;
        for (int y = 0; y < h; y++) {
            const uint32_t* s = src + (size_t)y * srcStride;
            uint32_t* d = dst + (size_t)y * stride;
            if (!(y & 1)) {
                if (mode == 3) for (int x = 0; x < w; x++) d[x] = (x & 1) ? shade(s[x], 60) : s[x];
                else memcpy(d, s, w * 4);
            } else {
                for (int x = 0; x < w; x++) d[x] = shade(s[x], (mode == 3 && (x & 1)) ? 35 : darkPct[mode]);
            }
        }
    } else {
        // Brightness of the three output rows per emulated line.
        static const uint32_t rowPct[4][3] = {{100, 100, 100}, {100, 95, 60}, {100, 85, 30}, {100, 100, 55}};
        lineAvg = (rowPct[mode][0] + rowPct[mode][1] + rowPct[mode][2]) / 300.f;
        if (mode == 3) lineAvg *= (2.f + 0.6f) / 3.f;
        for (int y = 0; y < h; y++) {
            const uint32_t* s = src + (size_t)y * srcStride;
            for (int r = 0; r < 3; r++) {
                uint32_t* d = dst + (size_t)(y * 3 + r) * stride;
                const uint32_t pct = rowPct[mode][r];
                if (mode == 3) {
                    for (int x = 0; x < w; x++) {
                        uint32_t p = pct == 100 ? s[x] : shade(s[x], pct);
                        d[x * 3] = p; d[x * 3 + 1] = p; d[x * 3 + 2] = shade(p, 60);
                    }
                } else if (pct == 100) {
                    memcpy(d, s, w * 4);
                } else {
                    for (int x = 0; x < w; x++) d[x] = shade(s[x], pct);
                }
            }
        }
    }

    // Hand it to the window, as SDR bytes or HDR half floats.
    bool wantHdr = g_hdr.load() && !g_hdrFailed.load() && hdr::setDataSpace();
    if (ww != g_winW || wh != g_winH || wantHdr != g_winHdr) {
        int32_t fmt = wantHdr ? AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT : WINDOW_FORMAT_RGBA_8888;
        if (ANativeWindow_setBuffersGeometry(g_window, ww, wh, fmt) != 0 && wantHdr) {
            LOG("HDR buffer format refused; staying in SDR");
            g_hdrFailed = true; wantHdr = false;
            ANativeWindow_setBuffersGeometry(g_window, ww, wh, WINDOW_FORMAT_RGBA_8888);
        }
        if (hdr::setDataSpace()) hdr::setDataSpace()(g_window, wantHdr ? hdr::DATASPACE_SCRGB_LINEAR : hdr::DATASPACE_UNKNOWN);
        g_winW = ww; g_winH = wh; g_winHdr = wantHdr;
    }
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(g_window, &buf, nullptr) != 0) {
        if (g_winHdr) { LOG("HDR buffer lock failed; staying in SDR"); g_hdrFailed = true; g_winW = 0; }
        return;
    }
    const int rows = std::min(wh, (int)buf.height), cols = std::min(ww, (int)buf.width);
    if (g_winHdr && buf.format == AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT) {
        if (!hdr::tablesReady) hdr::buildTables();
        const float peak = g_hdrPeak.load(), gain = 1.f / lineAvg;
        __fp16* out = static_cast<__fp16*>(buf.bits);
        for (int y = 0; y < rows; y++) hdr::convertRow(dst + (size_t)y * stride, out + (size_t)y * buf.stride * 4, cols, peak, gain);
    } else {
        uint32_t* out = static_cast<uint32_t*>(buf.bits);
        for (int y = 0; y < rows; y++) memcpy(out + (size_t)y * buf.stride, dst + (size_t)y * stride, cols * 4);
    }
    ANativeWindow_unlockAndPost(g_window);
}

// Shading data is needed for either enhancement. Caller holds g_emuLock.
void applyEnhancementsLocked() {
    if (g_emu) g_emu->setShadeTracking(g_smoothShade.load() || g_renderScale.load() == 2);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string& path, const uint8_t* data, size_t n) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = (fclose(f) == 0) && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

// Write the save chip to disk if the game changed it. Caller holds g_emuLock.
void flushEepromLocked() {
    if (!g_emu || g_eepromPath.empty() || !g_emu->eepromDirty) return;
    uint8_t e[128];
    g_emu->getEeprom(e);
    if (writeFileAtomic(g_eepromPath, e, sizeof e)) g_emu->eepromDirty = false;
    else LOG("could not write %s", g_eepromPath.c_str());
}

void flushMemoryTrackLocked() {
    if (!g_emu || !g_emu->cdUnit || !g_emu->memoryTrackDirty()) return;
    const std::string p = memoryTrackPath();
    if (p.empty()) return;
    uint8_t e[128];
    g_emu->getMemoryTrack(e);
    if (writeFileAtomic(p, e, sizeof e)) g_emu->clearMemoryTrackDirty();
    else LOG("could not write %s", p.c_str());
}

void flushNvmLocked() {
    if (!g_emu || !g_emu->cdUnit || !g_emu->nvmDirty) return;
    const std::string p = nvmPath();
    if (p.empty()) return;
    if (writeFileAtomic(p, g_emu->nvmData(), jag::Jaguar::NVM_SIZE)) g_emu->nvmDirty = false;
    else LOG("could not write %s", p.c_str());
}


// Compresses a save state into the on-disk "JDZ1" form.
static bool packState(const std::vector<uint8_t>& st, std::vector<uint8_t>& out) {
    uLongf clen = compressBound(st.size());
    out.assign(8 + clen, 0);
    memcpy(out.data(), "JDZ1", 4);
    uint32_t n = (uint32_t)st.size();
    memcpy(out.data() + 4, &n, 4);
    if (compress2(out.data() + 8, &clen, st.data(), st.size(), 6) != Z_OK) return false;
    out.resize(8 + clen);
    return true;
}

// Sound-engine watchdog: keeps a rolling set of one-second snapshots and, when the
// DSP has run off into non-DSP memory for a while (the sound then stays silent for
// good), saves the oldest snapshot so the lead-up can be replayed and fixed.
static std::deque<std::vector<uint8_t>> g_snaps;
static int g_ringTick = 0, g_dspLost = 0;
static bool g_crashSaved = false;
static void soundWatchdogLocked() {
    if (g_eepromPath.size() < 5) return;
    if (++g_ringTick >= 600) {
        g_ringTick = 0;
        if (!g_crashSaved) {   // stop rotating once a crash was caught
            g_snaps.push_back(g_emu->saveState());
            if (g_snaps.size() > 15) g_snaps.pop_front();
        }
    }
    uint32_t pc = g_emu->dsp.pc;
    bool lost = g_emu->dsp.running && pc >= 0xF00000 && !(pc >= 0xF1B000 && pc < 0xF1D000);
    g_dspLost = lost ? g_dspLost + 1 : 0;
    if (g_dspLost >= 60 && !g_crashSaved && !g_snaps.empty()) {
        g_crashSaved = true;
        std::vector<uint8_t> out;
        if (packState(g_snaps.front(), out))
            writeFileAtomic(g_eepromPath.substr(0, g_eepromPath.size() - 4) + ".dspcrash.state", out.data(), out.size());
    }
}

void emuLoop() {
    using clk = std::chrono::steady_clock;
    const auto frameTime = std::chrono::nanoseconds((long long)(1e9 / g_emu->refreshHz()));  // 59.94 or 50 Hz
    auto next = clk::now();
    auto fpsStart = clk::now();
    int fpsFrames = 0, sinceFlush = 0;
    g_fps = 0.f;
    while (g_run) {
        if (g_paused) { std::this_thread::sleep_for(std::chrono::milliseconds(16)); next = fpsStart = clk::now(); fpsFrames = 0; continue; }
        {
            std::lock_guard<std::mutex> lk(g_emuLock);
            g_emu->setButtons(g_buttons.load());
            g_emu->runFrame();
            present();
            soundWatchdogLocked();
            // Games write their save chip a few bits at a time; wait until it has
            // been quiet for a couple of seconds' worth of checks, then save.
            if (++sinceFlush >= 120) { sinceFlush = 0; flushEepromLocked(); flushMemoryTrackLocked(); flushNvmLocked(); }
        }
        if (++fpsFrames >= 15) {
            auto t = clk::now();
            double secs = std::chrono::duration<double>(t - fpsStart).count();
            if (secs > 0.25) { g_fps = (float)(fpsFrames / secs); fpsFrames = 0; fpsStart = t; }
        }

        int rate = g_emu->audioRate();
        // The sound card can drop the stream (route change, headset, system audio
        // reset) or silently stop pulling data; either way, nothing would ever
        // play again. Reopen it.
        if (g_stream) {
            static uint32_t lastR = 0; static int stuck = 0;
            const uint32_t rr = g_ringR.load(std::memory_order_relaxed);
            stuck = (rr == lastR && ringFill() > (uint32_t)(RING_FRAMES * 3 / 4)) ? stuck + 1 : 0;
            lastR = rr;
            aaudio_stream_state_t st = AAudioStream_getState(g_stream);
            if (g_audioDead.load() || stuck > 30 || st == AAUDIO_STREAM_STATE_DISCONNECTED || st == AAUDIO_STREAM_STATE_STOPPED || st == AAUDIO_STREAM_STATE_PAUSED) {
                LOG("audio stream lost (state %d, stuck %d); reopening", (int)st, stuck);
                stuck = 0; closeAudio();
            }
        }
        if (g_streamRate != OUT_RATE) openAudio(OUT_RATE);
        if (g_stream && !g_emu->audio.empty() && rate > 4000 && rate < 200000) {
            // Steer the queue toward TARGET_FILL: fuller than wanted -> consume input a
            // touch faster (fewer output frames), emptier -> a touch slower (as in v0.45).
            double err = ((double)ringFill() - TARGET_FILL) / TARGET_FILL;
            err = std::max(-1.0, std::min(1.0, err));
            g_resampler.process(g_emu->audio.data(), (int)g_emu->audio.size() / 2, rate, 1.0 + 0.01 * err);
            ringPush(g_resampler.out.data(), (int)g_resampler.out.size() / 2);
        }

        next += frameTime;
        auto now = clk::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > std::chrono::milliseconds(100)) next = now;  // fell behind: resync
    }
    closeAudio();
    std::lock_guard<std::mutex> lk(g_emuLock);
    flushEepromLocked();
    flushMemoryTrackLocked();
    flushNvmLocked();
}

void stopThread() {
    g_run = false;
    if (g_thread.joinable()) g_thread.join();
}

std::vector<uint8_t> toVec(JNIEnv* env, jbyteArray arr) {
    std::vector<uint8_t> v;
    if (!arr) return v;
    jsize n = env->GetArrayLength(arr);
    v.resize(n);
    env->GetByteArrayRegion(arr, 0, n, reinterpret_cast<jbyte*>(v.data()));
    return v;
}

std::string toStr(JNIEnv* env, jstring s) {
    if (!s) return std::string();
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_loadGame(JNIEnv* env, jclass, jbyteArray rom, jbyteArray bios) {
    stopThread();
    std::lock_guard<std::mutex> lk(g_emuLock);
    delete g_emu;
    g_emu = new jag::Jaguar();
    g_emu->pal = g_pal;
    g_error.clear();
    if (bios) {
        if (!g_emu->loadBios(toVec(env, bios), &g_error)) return JNI_FALSE;
    }
    if (!g_emu->loadCart(toVec(env, rom), &g_error)) return JNI_FALSE;
    std::vector<uint8_t> e;
    if (!g_eepromPath.empty() && readFile(g_eepromPath, e)) g_emu->setEeprom(e.data(), e.size());
    g_emu->reset();
    applyEnhancementsLocked();
    return JNI_TRUE;
}

// A Jaguar CD game: the CD BIOS runs from the cartridge slot and the disc image
// is a cue sheet whose track files Java has opened (Android storage hands out
// file descriptors, not paths).
JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_loadDisc(JNIEnv* env, jclass, jbyteArray cdBios, jstring cue,
                                                  jobjectArray names, jintArray fds) {
    stopThread();
    std::lock_guard<std::mutex> lk(g_emuLock);
    delete g_emu;
    g_emu = new jag::Jaguar();
    g_emu->pal = g_pal;
    g_error.clear();
    if (!g_emu->loadCart(toVec(env, cdBios), &g_error)) { g_error = "CD BIOS: " + g_error; return JNI_FALSE; }

    std::map<std::string, int> files;
    jsize n = env->GetArrayLength(names);
    jint* fd = env->GetIntArrayElements(fds, nullptr);
    for (jsize i = 0; i < n; i++) {
        auto js = (jstring)env->GetObjectArrayElement(names, i);
        const char* c = env->GetStringUTFChars(js, nullptr);
        files[c] = fd[i];
        env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
    }
    env->ReleaseIntArrayElements(fds, fd, JNI_ABORT);

    const char* cueText = env->GetStringUTFChars(cue, nullptr);
    std::string text = cueText;
    env->ReleaseStringUTFChars(cue, cueText);
    auto image = std::make_unique<jag::CdImage>();
    bool ok = image->loadText(text, [&](const std::string& name) {
        jag::CdImage::Opened o;
        auto it = files.find(name);
        if (it == files.end() || it->second < 0) return o;
        int d = dup(it->second);
        o.fp = d < 0 ? nullptr : fdopen(d, "rb");
        return o;
    }, &g_error);
    if (!ok) return JNI_FALSE;
    g_emu->insertDisc(std::move(image));
    { std::vector<uint8_t> mt; const std::string mp = memoryTrackPath(); if (!mp.empty() && readFile(mp, mt)) g_emu->setMemoryTrack(mt.data(), mt.size()); }
    { std::vector<uint8_t> nv; const std::string np = nvmPath(); if (!np.empty() && readFile(np, nv)) g_emu->setNvm(nv.data(), nv.size()); }
    std::vector<uint8_t> e;
    if (!g_eepromPath.empty() && readFile(g_eepromPath, e)) g_emu->setEeprom(e.data(), e.size());
    g_emu->reset();
    applyEnhancementsLocked();
    return JNI_TRUE;
}

// ---------------------------------------------------------------- zips --
std::atomic<float> g_unpackProgress{-1.f};   // 0..1 while a zipped disc unpacks, -1 otherwise
std::atomic<bool> g_unpackCancel{false};

static FILE* dupOpen(int fd) {
    int d = dup(fd);
    if (d < 0) return nullptr;
    FILE* f = fdopen(d, "rb");
    if (!f) close(d);
    return f;
}

// Names of the files inside a zip (for sorting zips into cartridges and discs).
JNIEXPORT jobjectArray JNICALL
Java_com_retroportlab_jagdroid_NativeLib_zipEntries(JNIEnv* env, jclass, jint fd) {
    jclass str = env->FindClass("java/lang/String");
    FILE* f = dupOpen(fd);
    jag::ZipArchive zip; std::string err;
    if (!f || !zip.open(f, &err)) { if (f) fclose(f); return env->NewObjectArray(0, str, nullptr); }
    auto& list = zip.entries();
    jobjectArray out = env->NewObjectArray((jsize)list.size(), str, nullptr);
    for (size_t i = 0; i < list.size(); i++) {
        jstring s = env->NewStringUTF(list[i].name.c_str());
        env->SetObjectArrayElement(out, (jsize)i, s);
        env->DeleteLocalRef(s);
    }
    fclose(f);
    return out;
}

JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_loadZipGame(JNIEnv* env, jclass, jint fd, jbyteArray bios) {
    std::vector<uint8_t> rom;
    std::string err;
    FILE* f = dupOpen(fd);
    bool ok = f && jag::readCartFromZip(f, rom, nullptr, &err);
    if (f) fclose(f);
    if (!ok) { std::lock_guard<std::mutex> lk(g_emuLock); g_error = err.empty() ? "Couldn't read the zip" : err; return JNI_FALSE; }
    jbyteArray arr = env->NewByteArray((jsize)rom.size());
    env->SetByteArrayRegion(arr, 0, (jsize)rom.size(), (const jbyte*)rom.data());
    jboolean r = Java_com_retroportlab_jagdroid_NativeLib_loadGame(env, nullptr, arr, bios);
    env->DeleteLocalRef(arr);
    return r;
}

// A zipped Jaguar CD (cue + bins). Blocking: call it off the UI thread and
// poll unpackProgress() for compressed discs, which are unpacked to cacheDir.
JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_loadZipDisc(JNIEnv* env, jclass, jbyteArray cdBios, jint fd, jstring cacheDir) {
    stopThread();
    const char* cd = env->GetStringUTFChars(cacheDir, nullptr);
    std::string cache = cd;
    env->ReleaseStringUTFChars(cacheDir, cd);
    std::vector<uint8_t> biosData = toVec(env, cdBios);
    g_unpackCancel = false;
    g_unpackProgress = 0.f;
    auto image = std::make_unique<jag::CdImage>();
    std::string err;
    bool ok = image->loadZip([fd]() { return dupOpen(fd); }, cache,
                             [](double f) { g_unpackProgress = (float)f; return !g_unpackCancel.load(); }, &err);
    g_unpackProgress = -1.f;
    std::lock_guard<std::mutex> lk(g_emuLock);
    if (!ok) { g_error = err; return JNI_FALSE; }
    delete g_emu;
    g_emu = new jag::Jaguar();
    g_emu->pal = g_pal;
    g_error.clear();
    if (!g_emu->loadCart(biosData, &g_error)) { g_error = "CD BIOS: " + g_error; return JNI_FALSE; }
    g_emu->insertDisc(std::move(image));
    { std::vector<uint8_t> mt; const std::string mp = memoryTrackPath(); if (!mp.empty() && readFile(mp, mt)) g_emu->setMemoryTrack(mt.data(), mt.size()); }
    { std::vector<uint8_t> nv; const std::string np = nvmPath(); if (!np.empty() && readFile(np, nv)) g_emu->setNvm(nv.data(), nv.size()); }
    std::vector<uint8_t> e;
    if (!g_eepromPath.empty() && readFile(g_eepromPath, e)) g_emu->setEeprom(e.data(), e.size());
    g_emu->reset();
    applyEnhancementsLocked();
    return JNI_TRUE;
}

JNIEXPORT jfloat JNICALL
Java_com_retroportlab_jagdroid_NativeLib_unpackProgress(JNIEnv*, jclass) { return g_unpackProgress.load(); }

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_cancelUnpack(JNIEnv*, jclass) { g_unpackCancel = true; }

JNIEXPORT jstring JNICALL
Java_com_retroportlab_jagdroid_NativeLib_lastError(JNIEnv* env, jclass) {
    return env->NewStringUTF(g_error.c_str());
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setSurface(JNIEnv* env, jclass, jobject surface) {
    std::lock_guard<std::mutex> lk(g_winLock);
    if (g_window) { ANativeWindow_release(g_window); g_window = nullptr; }
    if (surface) g_window = ANativeWindow_fromSurface(env, surface);
    g_winW = g_winH = 0;
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_start(JNIEnv*, jclass) {
    if (!g_emu || g_run) return;
    g_paused = false;
    g_run = true;
    g_thread = std::thread(emuLoop);
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setPaused(JNIEnv*, jclass, jboolean p) {
    g_paused = p;
    if (p) { std::lock_guard<std::mutex> lk(g_emuLock); flushEepromLocked(); }
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_stop(JNIEnv*, jclass) { stopThread(); }

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_reset(JNIEnv*, jclass) {
    bool was = g_run;
    stopThread();
    if (g_emu) g_emu->reset();
    if (was) { g_run = true; g_thread = std::thread(emuLoop); }
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setButtons(JNIEnv*, jclass, jint mask) { g_buttons = (uint32_t)mask; }

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setPal(JNIEnv*, jclass, jboolean pal) { g_pal = pal; }

JNIEXPORT jfloat JNICALL
Java_com_retroportlab_jagdroid_NativeLib_getFps(JNIEnv*, jclass) { return g_fps.load(); }

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setCropOverscan(JNIEnv*, jclass, jboolean on) { g_cropOverscan = on; }

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setEnhancements(JNIEnv*, jclass, jboolean smoothShade, jint scale) {
    g_smoothShade = smoothShade;
    g_renderScale = scale == 2 ? 2 : 1;
    std::lock_guard<std::mutex> lk(g_emuLock);
    applyEnhancementsLocked();
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setHdr(JNIEnv*, jclass, jboolean on, jfloat peak) {
    g_hdr = on;
    g_hdrPeak = std::max(1.f, std::min(8.f, (float)peak));
    if (on) g_hdrFailed = false;   // try again if the user turns it back on
}

JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_hdrActive(JNIEnv*, jclass) {
    return g_hdr.load() && !g_hdrFailed.load() && hdr::setDataSpace() != nullptr;
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setScanlines(JNIEnv*, jclass, jint mode) {
    g_scanlines = std::max(0, std::min(3, (int)mode));
    std::lock_guard<std::mutex> lk(g_winLock);
    g_winW = g_winH = 0;   // window buffer size changes with the mode
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_setEepromPath(JNIEnv* env, jclass, jstring path) {
    std::lock_guard<std::mutex> lk(g_emuLock);
    g_eepromPath = toStr(env, path);
    g_snaps.clear(); g_ringTick = 0; g_dspLost = 0; g_crashSaved = false;
}

// Writes the oldest rolling snapshot (about 30 s ago) for bug reports.
JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_saveSnapshot(JNIEnv* env, jclass, jstring path) {
    std::lock_guard<std::mutex> lk(g_emuLock);
    if (g_snaps.empty()) { g_error = "No earlier moment recorded yet"; return JNI_FALSE; }
    std::vector<uint8_t> out;
    if (!packState(g_snaps.front(), out)) { g_error = "Could not compress the snapshot"; return JNI_FALSE; }
    if (!writeFileAtomic(toStr(env, path), out.data(), out.size())) { g_error = "Could not write the snapshot"; return JNI_FALSE; }
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_retroportlab_jagdroid_NativeLib_flushSave(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_emuLock);
    flushEepromLocked();
    flushMemoryTrackLocked();
    flushNvmLocked();
}

// Save states are stored zlib-compressed: "JDZ1", original size, compressed data.
JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_saveState(JNIEnv* env, jclass, jstring path) {
    std::vector<uint8_t> st;
    {
        std::lock_guard<std::mutex> lk(g_emuLock);
        if (!g_emu) return JNI_FALSE;
        st = g_emu->saveState();
    }
    std::vector<uint8_t> out;
    if (!packState(st, out)) { g_error = "Could not compress the save state"; return JNI_FALSE; }
    if (!writeFileAtomic(toStr(env, path), out.data(), out.size())) { g_error = "Could not write the save state"; return JNI_FALSE; }
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_com_retroportlab_jagdroid_NativeLib_loadState(JNIEnv* env, jclass, jstring path) {
    std::vector<uint8_t> in;
    if (!readFile(toStr(env, path), in) || in.size() < 8 || memcmp(in.data(), "JDZ1", 4) != 0) {
        g_error = "No save state in this slot"; return JNI_FALSE;
    }
    uint32_t n; memcpy(&n, in.data() + 4, 4);
    if (n > 64u * 1024 * 1024) { g_error = "Save state is damaged"; return JNI_FALSE; }
    std::vector<uint8_t> st(n);
    uLongf dlen = n;
    if (uncompress(st.data(), &dlen, in.data() + 8, in.size() - 8) != Z_OK || dlen != n) {
        g_error = "Save state is damaged"; return JNI_FALSE;
    }
    std::lock_guard<std::mutex> lk(g_emuLock);
    if (!g_emu) return JNI_FALSE;
    std::string err;
    if (!g_emu->loadState(st, &err)) { g_error = err; return JNI_FALSE; }
    return JNI_TRUE;
}

// The current picture at half size, for save-slot thumbnails: [width, height, ARGB...].
JNIEXPORT jintArray JNICALL
Java_com_retroportlab_jagdroid_NativeLib_getThumbnail(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lk(g_emuLock);
    if (!g_emu) return nullptr;
    int w = g_emu->frameWidth() / 2, h = g_emu->frameHeight() / 2;
    std::vector<jint> px(2 + (size_t)w * h);
    px[0] = w; px[1] = h;
    const uint32_t* fb = g_emu->framebuffer();
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = fb[(size_t)(y * 2) * jag::Jaguar::MAX_W + x * 2];   // RGBA, R low
            px[2 + y * w + x] = (jint)(0xFF000000u | ((p & 0xFF) << 16) | (p & 0xFF00) | ((p >> 16) & 0xFF));
        }
    jintArray arr = env->NewIntArray((jsize)px.size());
    env->SetIntArrayRegion(arr, 0, (jsize)px.size(), px.data());
    return arr;
}

} // extern "C"

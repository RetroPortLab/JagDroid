// Headless test runner: jagrun <cart> [--bios file] [--frames N] [--out frame.ppm] [-v]
#include "jaguar.h"
#include "zipfile.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

extern "C" {
#include "m68k.h"
}

static std::vector<uint8_t> slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: jagrun <cart> [--bios file] [--frames N] [--out frame.ppm] [-v]\n");
        return 1;
    }
    const char* cart = argv[1];
    const char* biosPath = nullptr;
    const char* outPath = "frame.ppm";
    int frames = 120;
    bool verbose = false, pal = false;
    const char* eepromPath = nullptr;
    const char* nvmPath = nullptr;        // --nvm file: the Memory Track cartridge (128 KB)
    const char* memTrackPath = nullptr;   // --memtrack file: the CD unit save chip (128 bytes)
    const char* cuePath = nullptr;      // Jaguar CD disc (the cart argument is then the CD BIOS)   // load before start, save at exit if the game wrote it
    const char* wavPath = nullptr;      // --wav file: write the sound output
    int smooth = 0;                     // --smooth N: also write the enhanced picture at N x (needs shade tracking)
    int stateTest = -1;                 // save a state at this frame, then check a reload replays identically
    struct Hold { uint32_t mask; int from, to; };
    std::vector<Hold> holds;   // --hold <mask> <fromFrame> <toFrame>
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--nvm") && i + 1 < argc) nvmPath = argv[++i];
        else if (!strcmp(argv[i], "--memtrack") && i + 1 < argc) memTrackPath = argv[++i];
        else if (!strcmp(argv[i], "--bios") && i + 1 < argc) biosPath = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) outPath = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "--pal")) pal = true;
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) eepromPath = argv[++i];
        else if (!strcmp(argv[i], "--cd") && i + 1 < argc) cuePath = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wavPath = argv[++i];
        else if (!strcmp(argv[i], "--smooth") && i + 1 < argc) smooth = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--state-test") && i + 1 < argc) stateTest = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 3 < argc) {
            holds.push_back({(uint32_t)strtoul(argv[i + 1], nullptr, 0), atoi(argv[i + 2]), atoi(argv[i + 3])});
            i += 3;
        }
    }

    jag::Jaguar* j = new jag::Jaguar();
    j->verbose = verbose;
    j->pal = pal;
    std::string err;
    jag::pcWatchOn = getenv("JAG_PCWATCH") != nullptr;
    if (biosPath && !j->loadBios(slurp(biosPath), &err)) { fprintf(stderr, "BIOS: %s\n", err.c_str()); return 1; }
    std::vector<uint8_t> cartData;
    if (jag::endsWithNoCase(cart, ".zip")) {
        FILE* zf = fopen(cart, "rb"); std::string inner;
        if (!zf || !jag::readCartFromZip(zf, cartData, &inner, &err)) { fprintf(stderr, "Zip: %s\n", err.c_str()); return 1; }
        fclose(zf); printf("cart from zip: %s\n", inner.c_str());
    } else cartData = slurp(cart);
    if (!j->loadCart(cartData, &err)) { fprintf(stderr, "Cart: %s\n", err.c_str()); return 1; }
    if (eepromPath) { auto e = slurp(eepromPath); j->setEeprom(e.data(), e.size()); }
    if (cuePath && jag::endsWithNoCase(cuePath, ".zip")) {
        auto img = std::make_unique<jag::CdImage>();
        std::string zp = cuePath; int lastPct = -1;
        bool ok = img->loadZip([zp]() { return fopen(zp.c_str(), "rb"); }, "/tmp/claude-0/jagcd_cache",
                               [&](double f) { int p = (int)(f * 100); if (p / 10 != lastPct / 10) { printf("unpacking %d%%\n", p); fflush(stdout); } lastPct = p; return true; }, &err);
        if (!ok) { fprintf(stderr, "CD zip: %s\n", err.c_str()); return 1; }
        j->insertDisc(std::move(img));
    } else if (cuePath && !j->insertDisc(cuePath, &err)) { fprintf(stderr, "CD: %s\n", err.c_str()); return 1; }
    if (cuePath) for (auto& t : j->disc->tracks) printf("track %2d session %d start %d frames %d\n", t.number, t.session, t.start, t.frames);
    if (nvmPath) { auto nv = slurp(nvmPath); j->setNvm(nv.data(), nv.size()); }
    if (memTrackPath) { auto mt = slurp(memTrackPath); if (!mt.empty()) j->setMemoryTrack(mt.data(), mt.size()); }
    j->reset();
    if (smooth > 0) j->setShadeTracking(true);
    std::vector<uint16_t> raw(jag::Jaguar::MAX_W * jag::Jaguar::MAX_H);
    if (getenv("JAG_RAW") || getenv("JAG_PROBE")) j->rawOut = raw.data();

    size_t audioSamples = 0;
    std::vector<int16_t> wav;
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> state;
    auto fbHash = [&]() { uint64_t h = 1469598103934665603ull; for (int y = 0; y < j->frameHeight(); y++) for (int x = 0; x < j->frameWidth(); x++) { h ^= j->framebuffer()[y * jag::Jaguar::MAX_W + x]; h *= 1099511628211ull; } return h; };
    for (int f = 0; f < frames; f++) {
        if (f == stateTest) { state = j->saveState(); printf("state saved at frame %d: %zu bytes\n", f, state.size()); }
        uint32_t m = 0;
        for (auto& h : holds) if (f >= h.from && f < h.to) m |= h.mask;
        j->setButtons(m);
        j->runFrame();
        if (const char* db = getenv("JAG_DUMPBUF")) {   // "frame:addr(hex):widthPixels:height:file.ppm" 16-bit CRY/RGB buffer
            long bf; unsigned ba; int bw, bh; char bp[200];
            if (sscanf(db, "%ld:%x:%d:%d:%199s", &bf, &ba, &bw, &bh, bp) == 5 && f == bf) {
                FILE* o = fopen(bp, "wb"); fprintf(o, "P6\n%d %d\n255\n", bw, bh);
                for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) {
                    uint16_t v = j->read16(ba + (y * bw + x) * 2);
                    uint32_t px = j->cryTable[v];
                    fputc(px & 255, o); fputc((px >> 8) & 255, o); fputc((px >> 16) & 255, o);
                }
                fclose(o);
            }
        }
        if (const char* dm = getenv("JAG_DUMPMEM")) {   // "frame:addr(hex):len(hex):file" raw 68k-space bytes
            long mf; unsigned ma, ml; char mp[200];
            if (sscanf(dm, "%ld:%x:%x:%199s", &mf, &ma, &ml, mp) == 4 && f == mf) { FILE* o = fopen(mp, "wb"); for (unsigned k = 0; k < ml; k++) fputc(j->read8(ma + k), o); fclose(o); }
        }
        if (getenv("JAG_RATELOG")) {   // report audio-rate changes
            static int lastRate = -1; int r = j->audioRate();
            if (r != lastRate) { fprintf(stderr, "[rate] frame %d: %d Hz (SMODE=%04X SCLK=%04X)\n", f, r, j->jerryReg16(0x1D4 + 0 + 0x0) , 0); lastRate = r; }
        }
        if (const char* df = getenv("JAG_DUMPFRAMES")) {   // "from:to:prefix" writes each frame as prefix_NNNNN.ppm
            static long dfrom = -1, dto = -1; static char pre[256];
            if (dfrom < 0) sscanf(df, "%ld:%ld:%255s", &dfrom, &dto, pre);
            static int dstep = getenv("JAG_DUMPSTEP") ? atoi(getenv("JAG_DUMPSTEP")) : 1;
            if (f >= dfrom && f <= dto && (f - dfrom) % dstep == 0) {
                char nm[300]; snprintf(nm, sizeof nm, "%s_%05d.ppm", pre, f);
                if (FILE* o = fopen(nm, "wb")) {
                    int w = j->frameWidth(), h = j->frameHeight();
                    fprintf(o, "P6\n%d %d\n255\n", w, h);
                    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                        uint32_t p = j->framebuffer()[y * jag::Jaguar::MAX_W + x];
                        uint8_t rgb[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)};
                        fwrite(rgb, 1, 3, o);
                    }
                    fclose(o);
                }
            }
        }
        if (const char* pl = getenv("JAG_PCLOG")) { if (f % atoi(pl) == 0) fprintf(stderr, "[pc] f%d PC=%06X SR=%04X gpu=%d dsp=%d blits=%llu\n", f, m68k_get_reg(nullptr, M68K_REG_PC), m68k_get_reg(nullptr, M68K_REG_SR), j->gpu.running, j->dsp.running, (unsigned long long)j->blitCount); if (getenv("JAG_PCBYTES") && f % atoi(pl) == 0) { unsigned pc = m68k_get_reg(nullptr, M68K_REG_PC); fprintf(stderr, "   code@%06X:", pc - 8); for (int k = -8; k < 24; k += 2) fprintf(stderr, " %04X", j->read16(pc + k)); fprintf(stderr, "\n"); unsigned sp = m68k_get_reg(nullptr, M68K_REG_SP); { unsigned nz=0, lastnz=0; for (unsigned a = 0x4000; a < 0x70000; a += 4) if (j->read32(a)) { nz++; lastnz = a; } fprintf(stderr, "   load: nonzero longs=%u last=%06X  @4000:", nz, lastnz); for (int k = 0; k < 40; k += 4) fprintf(stderr, " %08X", j->read32(0x4000 + k)); fprintf(stderr, "\n   @4018:"); for (int k = 0; k < 32; k += 4) fprintf(stderr, " %08X", j->read32(0x4018 + k)); fprintf(stderr, "\n"); } fprintf(stderr, "   vec:"); for (int k = 8; k < 0x40; k += 4) fprintf(stderr, " %02X=%06X", k, j->read32(k)); fprintf(stderr, "\n   sp=%06X:", sp); for (int k = 0; k < 40; k += 4) fprintf(stderr, " %08X", j->read32(sp + k)); fprintf(stderr, "\n"); } }
        audioSamples += j->audio.size() / 2;
        if (wavPath) wav.insert(wav.end(), j->audio.begin(), j->audio.end());
    }
    if (!state.empty()) {
        uint64_t a = fbHash(); uint64_t dramA = 0; for (uint32_t k = 0; k < jag::Jaguar::DRAM_SIZE; k++) dramA = dramA * 31 + j->dram[k];
        std::string e;
        if (!j->loadState(state, &e)) printf("loadState failed: %s\n", e.c_str());
        for (int f = stateTest; f < frames; f++) {
            uint32_t m = 0;
            for (auto& h : holds) if (f >= h.from && f < h.to) m |= h.mask;
            j->setButtons(m); j->runFrame();
        }
        uint64_t b = fbHash(); uint64_t dramB = 0; for (uint32_t k = 0; k < jag::Jaguar::DRAM_SIZE; k++) dramB = dramB * 31 + j->dram[k];
        printf("state replay: picture %s, RAM %s\n", a == b ? "identical" : "DIFFERENT", dramA == dramB ? "identical" : "DIFFERENT");
    }
    if (eepromPath && j->eepromDirty) {
        uint8_t e[128]; j->getEeprom(e);
        if (FILE* ef = fopen(eepromPath, "wb")) { fwrite(e, 1, 128, ef); fclose(ef); printf("EEPROM saved to %s\n", eepromPath); }
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    printf("frames=%d time=%.3fs (%.1f fps) size=%dx%d\n", frames, secs, frames / secs,
           j->frameWidth(), j->frameHeight());
    printf("68k PC=$%06X  GPU running=%d instr=%llu  DSP running=%d instr=%llu  blits=%llu  audio=%zu samples @%d Hz\n",
           m68k_get_reg(nullptr, M68K_REG_PC), j->gpu.running, (unsigned long long)j->gpu.instrCount,
           j->dsp.running, (unsigned long long)j->dsp.instrCount, (unsigned long long)j->blitCount,
           audioSamples, j->audioRate());
    if (nvmPath && j->nvmDirty) { if (FILE* o = fopen(nvmPath, "wb")) { fwrite(j->nvmData(), 1, jag::Jaguar::NVM_SIZE, o); fclose(o); printf("memory track cartridge saved to %s\n", nvmPath); } }
    if (memTrackPath && j->memoryTrackDirty()) { uint8_t e[128]; j->getMemoryTrack(e); if (FILE* o = fopen(memTrackPath, "wb")) { fwrite(e, 1, sizeof e, o); fclose(o); printf("memory track saved to %s\n", memTrackPath); } }
    if (j->cdUnit && verbose) j->cd.debugDump();

    if (wavPath) {
        if (FILE* wf = fopen(wavPath, "wb")) {
            uint32_t rate = j->audioRate(), bytes = (uint32_t)wav.size() * 2;
            auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, wf); }; auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, wf); };
            fwrite("RIFF", 1, 4, wf); u32(36 + bytes); fwrite("WAVEfmt ", 1, 8, wf); u32(16); u16(1); u16(2);
            u32(rate); u32(rate * 4); u16(4); u16(16); fwrite("data", 1, 4, wf); u32(bytes);
            fwrite(wav.data(), 2, wav.size(), wf); fclose(wf);
        }
    }
    if (smooth > 0) {
        int w = j->frameWidth() * smooth, h = j->frameHeight() * smooth;
        std::vector<uint32_t> img((size_t)w * h);
        j->renderSmooth(smooth, img.data());
        std::string sp = std::string(outPath) + ".smooth.ppm";
        if (FILE* so = fopen(sp.c_str(), "wb")) {
            fprintf(so, "P6\n%d %d\n255\n", w, h);
            for (uint32_t p : img) { uint8_t rgb[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)}; fwrite(rgb, 1, 3, so); }
            fclose(so);
        }
    }
    FILE* o = fopen(outPath, "wb");
    if (o) {
        int w = j->frameWidth(), h = j->frameHeight();
        fprintf(o, "P6\n%d %d\n255\n", w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint32_t p = j->framebuffer()[y * jag::Jaguar::MAX_W + x];
                uint8_t rgb[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)};
                fwrite(rgb, 1, 3, o);
            }
        fclose(o);
    }
    if (const char* probe = getenv("JAG_PROBE")) {   // "x,y x,y ..." raw values at points
        int px, py, n;
        while (sscanf(probe, "%d,%d%n", &px, &py, &n) == 2) {
            uint16_t v = raw[py * jag::Jaguar::MAX_W + px];
            uint32_t c = j->framebuffer()[py * jag::Jaguar::MAX_W + px];
            printf("probe %d,%d raw=%04X rgb=%02X%02X%02X\n", px, py, v, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
            probe += n; while (*probe == ' ') probe++;
        }
    }
    if (getenv("JAG_RAW")) {   // histogram of raw pixel values in the last frame
        std::vector<int> hist(65536);
        for (int y = 0; y < j->frameHeight(); y++)
            for (int x = 0; x < j->frameWidth(); x++) hist[raw[y * jag::Jaguar::MAX_W + x]]++;
        std::vector<std::pair<int,int>> v;
        for (int i = 0; i < 65536; i++) if (hist[i]) v.push_back({hist[i], i});
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 12; i++) printf("raw %04X x%d\n", v[i].second, v[i].first);
    }
    delete j;
    return 0;
}

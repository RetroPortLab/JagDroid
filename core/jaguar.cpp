// JagDroid - machine glue: memory bus, 68000 hookup, interrupts, scheduler.
#include "jaguar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include "m68k.h"
}

#ifdef JAG_TRACE
extern "C" void jag_trace_write(unsigned addr, unsigned val, int size);
#define TRACE_W(a, v, s) jag_trace_write(a, v, s)
#else
#define TRACE_W(a, v, s) ((void)0)
#endif

namespace jag {

Jaguar* g_jag = nullptr;

// TOM register offsets (relative to $F00000)
enum : uint32_t {
    T_MEMCON1 = 0x00, T_MEMCON2 = 0x02, T_HC = 0x04, T_VC = 0x06,
    T_OLP = 0x20, T_OBF = 0x26, T_VMODE = 0x28, T_BORD1 = 0x2A, T_BORD2 = 0x2C,
    T_HP = 0x2E, T_HBB = 0x30, T_HBE = 0x32, T_HDB1 = 0x38, T_HDB2 = 0x3A, T_HDE = 0x3C,
    T_VP = 0x3E, T_VBB = 0x40, T_VBE = 0x42, T_VS = 0x44, T_VDB = 0x46, T_VDE = 0x48,
    T_VI = 0x4E, T_PIT0 = 0x50, T_PIT1 = 0x52, T_BG = 0x58,
    T_INT1 = 0xE0, T_INT2 = 0xE2,
    T_GPU_REGS = 0x2100, T_GPU_REGS_END = 0x2120,
    T_BLIT = 0x2200, T_BLIT_CMD = 0x2238, T_BLIT_END = 0x22A0,
    T_GPU_RAM = 0x3000,
};
// JERRY register offsets (relative to $F10000)
enum : uint32_t {
    J_PIT1 = 0x00, J_PIT2 = 0x02, J_PIT3 = 0x04, J_PIT4 = 0x06,
    J_INT = 0x20, J_JOYSTICK = 0x4000, J_JOYBUTS = 0x4002,
    J_DSP_REGS = 0xA100, J_DSP_REGS_END = 0xA124,
    J_LTXD = 0xA148, J_RTXD = 0xA14C, J_SCLK = 0xA150, J_SMODE = 0xA154,
    J_DSP_RAM = 0xB000,
};

Jaguar::Jaguar() {
    g_jag = this;
    dram = new uint8_t[DRAM_SIZE];
    fb = new uint32_t[MAX_W * MAX_H];
    gpu.jag = this; gpu.isDsp = false; gpu.name = "GPU";
    gpu.ramBase = 0xF03000; gpu.ramSize = 0x1000; gpu.ram = &tom[T_GPU_RAM];
    dsp.jag = this; dsp.isDsp = true; dsp.name = "DSP";
    dsp.ramBase = 0xF1B000; dsp.ramSize = 0x2000; dsp.ram = &jerry[J_DSP_RAM];
    for (auto& w : eeprom) w = 0xFFFF;
    cd.jag = this;
    buildCryTable();
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    reset();
}

Jaguar::~Jaguar() {
    setShadeTracking(false);
    delete[] dram;
    delete[] fb;
    if (g_jag == this) g_jag = nullptr;
}

bool Jaguar::loadCart(const std::vector<uint8_t>& data, std::string* err) {
    if (data.size() < 0x2000) { if (err) *err = "File is too small to be a Jaguar cartridge"; return false; }
    if (data.size() > ROM_MAX) { if (err) *err = "File is larger than the 6 MB cartridge space"; return false; }
    rom = data;
    // Some dumps carry an 8 KB header (".j64" made from ".rom" variants). A real
    // cart has its boot info at $400; if it looks absent but present 8 KB later,
    // strip the header.
    auto be32 = [&](size_t o) { return (uint32_t)rom[o] << 24 | rom[o+1] << 16 | rom[o+2] << 8 | rom[o+3]; };
    uint32_t start = be32(0x404);
    if ((start < 0x800000 || start >= 0xE00000) && rom.size() > 0x2404) {
        uint32_t alt = be32(0x2404);
        if (alt >= 0x800000 && alt < 0xE00000) rom.erase(rom.begin(), rom.begin() + 0x2000);
    }
    romChecksum = 2166136261u;   // FNV-1a, ties save states to their game
    for (uint8_t b : rom) { romChecksum ^= b; romChecksum *= 16777619u; }
    return true;
}

bool Jaguar::insertDisc(const std::string& cuePath, std::string* err) {
    auto d = std::make_unique<CdImage>();
    if (!d->load(cuePath, err)) return false;
    insertDisc(std::move(d));
    return true;
}

void Jaguar::insertDisc(std::unique_ptr<CdImage> image) {
    disc = std::move(image);
    cd.disc = disc.get();
    cdUnit = true;
    // Tie save states to the disc as well as the CD BIOS.
    for (auto& t : disc->tracks) { romChecksum ^= (uint32_t)t.frames + 0x9E3779B9u * t.number; romChecksum *= 16777619u; }
}

bool Jaguar::loadBios(const std::vector<uint8_t>& data, std::string* err) {
    if (data.size() != BIOS_SIZE) { if (err) *err = "BIOS must be exactly 128 KB"; return false; }
    bios = data;
    hasBios = true;
    return true;
}

// Horizontal count advances through the line as the scheduler's slices run,
// so code that waits for it to change always sees it move.
// Within a slice it keeps counting with each read (3 ticks per read, so the
// low bits run through every value and never go backwards on the line):
// Atari Karts picks random opponents with a loop that waits for HC & 7 to
// land on particular values, which a counter frozen per slice never showed.
uint16_t Jaguar::hcount() const {
    int hp = tomReg16(T_HP) & 0x3FF;
    if (hp < 100) hp = 844;
    const int width = 2 * (hp + 1) / 8;                 // HC span of one slice
    const uint32_t key = (uint32_t)halfLine << 4 | (uint32_t)lineSlice;
    if (key != hcKey) { hcKey = key; hcReads = 0; }
    int extra = std::min(hcReads * 3, width - 1);
    hcReads++;
    return (uint16_t)((lineSlice * width + extra) & 0x7FF);
}

int Jaguar::linesPerFrame() const {
    int vp = tomReg16(T_VP) & 0x7FF;
    if (vp > 400 && vp < 700) return (vp + 1) / 2;
    return pal ? 313 : 263;
}

bool Jaguar::i2sExternal() const { return cdUnit && !(jerryReg16(J_SMODE + 2) & 1); }

// One I2S frame clocked in by the CD unit (it drives the word clock when JERRY's
// serial port is in slave mode).
void Jaguar::cdI2sFrame(uint16_t w0, uint16_t w1) {
    if (!i2sExternal()) return;
    // JERRY's interrupt lands after the left word of the new frame has been
    // shifted in, while the right register still holds the previous frame's.
    i2sRxL = w0; i2sRxR = i2sPrevR; i2sPrevR = w1;
    audio.push_back((int16_t)jerryReg16(J_LTXD + 2));
    audio.push_back((int16_t)jerryReg16(J_RTXD + 2));
    if (dsp.running) dsp.raiseInt(1);
}

int Jaguar::audioRate() const {
    if (i2sExternal()) return 44100 * cd.speed;
    int sclk = jerryReg16(J_SCLK + 2) & 0xFF;
    return (int)(clock() / (64.0 * (sclk + 1)));
}

// JERRY's wavetable ROM ($F1D000-$F1DFFF): eight 128-entry tables of 16-bit
// samples, sign-extended to 32 bits. The BIOS start-up jingle and some games
// synthesise notes from these. The tables are generated here from the shapes
// they hold rather than copied from a ROM dump; the two noise tables only need
// to be noise.
static void fillWaveRom(uint8_t* rom) {
    const double PI = 3.14159265358979323846;
    uint32_t seed = 0x2545F491u;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (int)(seed % 65536u) - 32768; };
    for (int t = 0; t < 8; t++)
        for (int i = 0; i < 128; i++) {
            const double x = 2 * PI * (i + 1) / 128;
            const int tri = i < 64 ? 512 * i - 15871 : 15871 - 512 * (i - 64);
            double v = 0;
            switch (t) {
            case 0: v = tri; break;                                                // triangle
            case 1: v = 32767 * std::sin(x); break;                                // sine
            case 2: v = 32767 * std::sin(x) * (i + 1) / 100.0; break;              // amplitude-modulated sine
            case 3: v = 16383 * (std::sin(x) + std::sin(2 * x)); break;            // sine + second harmonic
            case 4: v = 32767 * std::sin(0.061 + 0.06025 * i + 2 * PI * 15 / 16384 * i * i); break;   // chirp
            case 5: v = tri + 500 + rnd() / 48; break;                             // triangle with noise
            case 6: v = (i >= 60 && i <= 66) ? 8192 * (4 - std::abs(i - 63)) - 1 : 0; break;   // spike
            case 7: v = rnd() / 5; break;                                          // white noise
            }
            int32_t s = (int32_t)std::max(-32767.0, std::min(32767.0, v));
            uint8_t* o = rom + t * 512 + i * 4;
            o[0] = (uint8_t)(s >> 24); o[1] = (uint8_t)(s >> 16); o[2] = (uint8_t)(s >> 8); o[3] = (uint8_t)s;
        }
}

void Jaguar::reset() {
    memset(dram, 0, DRAM_SIZE);
    memset(tom, 0, sizeof(tom));
    memset(jerry, 0, sizeof(jerry));
    fillWaveRom(&jerry[0xD000]);
    memset(fb, 0, sizeof(uint32_t) * MAX_W * MAX_H);
    memset(lineBuf, 0, sizeof(lineBuf));
    memset(lineBuf32, 0, sizeof(lineBuf32));
    gpu.reset(); dsp.reset();
    tomIntEnable = tomIntPending = jerryIntEnable = jerryIntPending = 0;
    tomPitAcc = jPit1Acc = jPit2Acc = i2sAcc = 0;
    joySelect = 0xFFFF;
    halfLine = 0;
    frameCount = 0;
    blitBusy = 0; blitIntPending = false;
    eeState = 0; eeBits = 0; eeWriteEnable = false;
    cd.reset();

    // Sensible NTSC video defaults (games normally program their own).
    auto setT = [&](uint32_t off, uint16_t v) { tom[off] = v >> 8; tom[off + 1] = v & 0xFF; };
    setT(T_MEMCON1, 0x1861); setT(T_MEMCON2, 0x35CC);
    setT(T_HP, 844); setT(T_HBB, 1713); setT(T_HBE, 125);
    setT(T_HDB1, 203); setT(T_HDB2, 203); setT(T_HDE, 1665);
    setT(T_VP, 523); setT(T_VBB, 500); setT(T_VBE, 24); setT(T_VS, 517);
    setT(T_VDB, 38); setT(T_VDE, 518); setT(T_VI, 0xFFFF);
    setT(T_VMODE, 0x06C7);  // RGB16, PWIDTH 4, video enabled, BGEN
    if (pal) { setT(T_VP, 625); setT(T_VBB, 600); setT(T_VBE, 34); setT(T_VS, 619); setT(T_VDB, 46); setT(T_VDE, 600); }
    // DSP serial clock default (about 20.8 kHz)
    jerry[J_SCLK + 3] = 19;

    m68k_pulse_reset();
    if (hasBios) {
        // At reset the BIOS is visible at address 0 for the vector fetch.
        auto b32 = [&](int o) { return (uint32_t)bios[o] << 24 | bios[o+1] << 16 | bios[o+2] << 8 | bios[o+3]; };
        m68k_set_reg(M68K_REG_SP, b32(0));
        m68k_set_reg(M68K_REG_A7, b32(0));
        m68k_set_reg(M68K_REG_PC, b32(4));
    } else {
        bootWithoutBios();
    }
}

// High-level boot: do the minimum the BIOS would leave behind, then jump to the
// cartridge's start address (stored at $800404).
void Jaguar::bootWithoutBios() {
    // Our helper data lives in the (empty) BIOS address space, never in DRAM,
    // because games own all of DRAM (Cybermorph keeps its stack and a GPU
    // semaphore right at the top of it).
    bios.assign(BIOS_SIZE, 0);
    const uint32_t stopObj = BIOS_BASE + 0x10;
    bios[0x10 + 7] = 4;  // type 4 = STOP
    // OLP is stored word-swapped
    tom[T_OLP + 0] = (stopObj >> 8) & 0xFF; tom[T_OLP + 1] = stopObj & 0xFF;
    tom[T_OLP + 2] = (stopObj >> 24) & 0xFF; tom[T_OLP + 3] = (stopObj >> 16) & 0xFF;

    // Exception vectors point at RTEs so stray interrupts don't crash: one per
    // vector ($E00100 + 2 * vector), so a debugger can tell which one fired.
    bios[0x20] = 0x4E; bios[0x21] = 0x73;
    for (int v = 2; v < 256; v++) {
        const uint32_t rteAddr = BIOS_BASE + 0x100 + v * 2;
        bios[0x100 + v * 2] = 0x4E; bios[0x100 + v * 2 + 1] = 0x73;   // RTE
        uint32_t a = v * 4;
        dram[a] = rteAddr >> 24; dram[a+1] = (rteAddr >> 16) & 0xFF;
        dram[a+2] = (rteAddr >> 8) & 0xFF; dram[a+3] = rteAddr & 0xFF;
    }

    uint32_t start = 0x802000;
    if (rom.size() >= 0x408) {
        uint32_t s = read32(0x800404);
        if (s >= 0x800000 && s < 0xE00000) start = s;
        else if (s < DRAM_SIZE && s >= 0x4000) start = s;
    }
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_SP, 0x00004000);
    m68k_set_reg(M68K_REG_A7, 0x00004000);
    m68k_set_reg(M68K_REG_PC, start);
    if (verbose) fprintf(stderr, "[boot] no BIOS, starting cart at $%06X\n", start);
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
// Interrupts are only latched while their enable bit is set; an event that
// happens while disabled is lost, so it can't fire later when the game
// enables it before installing a handler.
void Jaguar::tomRaise(int bit) {
    static const char* intLog = getenv("JAG_INTLOG");
    if (intLog && (long)frameCount >= atol(intLog) && (long)frameCount < atol(intLog) + 8)
        fprintf(stderr, "[tomint] bit %d frame %llu VC=%d en=%02X\n", bit, (unsigned long long)frameCount, halfLine, tomIntEnable);
    if (!(tomIntEnable & (1u << bit))) return;
    tomIntPending |= (1u << bit);
    updateM68kIrq();
}

void Jaguar::jerryRaise(int bit) {
    if (!(jerryIntEnable & (1u << bit))) return;
    jerryIntPending |= (1u << bit);
    if (jerryIntPending & jerryIntEnable) tomRaise(4);  // JERRY feeds TOM's external int
}

void Jaguar::updateM68kIrq() {
    bool active = (tomIntPending & tomIntEnable & 0x1F) != 0;
    m68k_set_irq(active ? 2 : 0);
}

// ---------------------------------------------------------------------------
// Memory bus (big-endian)
// ---------------------------------------------------------------------------
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool Jaguar::tomReadHook(uint32_t off, int size, uint32_t& out) {
    // Beam counters. A long read at $F00004 returns HC:VC; at $F00002 MEMCON2:HC.
    if (off == T_HC) {
        out = (size == 4) ? ((uint32_t)hcount() << 16 | (uint16_t)halfLine) : hcount();
        return true;
    }
    if (size == 4 && off == T_MEMCON2) {
        out = (uint32_t)tomReg16(T_MEMCON2) << 16 | hcount();
        return true;
    }
    if (off == T_VC) { out = (uint16_t)halfLine; return true; }
    if (off == T_INT1) { out = tomIntPending; return true; }
    if (off >= T_GPU_REGS && off < T_GPU_REGS_END) {
        uint32_t v = gpu.readCtrlReg((off - T_GPU_REGS) & ~3u);
        out = (size == 4) ? v : (size == 2 ? ((off & 2) ? (v & 0xFFFF) : (v >> 16))
                                           : (v >> (8 * (3 - (off & 3)))) & 0xFF);
        return true;
    }
    if (off == T_BLIT_CMD || off == T_BLIT_CMD + 2) {
        uint32_t idle = blitBusy > 0 ? 0 : 1;   // bit 0: blitter idle
        out = (size == 4) ? idle : ((off & 2) ? idle : 0);
        return true;
    }
    return false;
}

bool Jaguar::jerryReadHook(uint32_t off, int size, uint32_t& out) {
    if (off == J_INT) { out = jerryIntPending; return true; }
    if (off == 0x3A) {   // JERRY's random number generator: a new 16-bit value on every read
        auto next = [&]() { rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5; return (uint16_t)(rngState >> 8); };
        const uint16_t a = next();
        out = (size == 4) ? ((uint32_t)a << 16 | next()) : a;
        return true;
    }
    if (cdUnit && (off & ~7u) == J_LTXD) {   // I2S receive (LRXD/RRXD): data from the CD unit
        uint32_t v = (off & 4) ? i2sRxR : i2sRxL;
        out = (size == 4) ? v : ((off & 2) ? v : 0);
        return true;
    }
    if (off == J_JOYSTICK && verbose && getenv("JAG_JOYLOG") && (long)frameCount == atol(getenv("JAG_JOYLOG"))) {
        static int n = 0;
        if (n++ < 40) fprintf(stderr, "[joy] read size %d by %d sel=%04X\n", size, busMaster, joySelect);
    }
    if (off == J_JOYSTICK) {
        uint16_t js = (uint16_t)((readJoystick() & 0xFFFE) | eeGetDO());   // bit 0: EEPROM data out
        out = (size == 4) ? ((uint32_t)js << 16 | readJoyButtons()) : js;
        return true;
    }
    if (off == 0x5000) { eeSetCS(); out = 0; return true; }   // EEPROM chip select
    if (off == J_JOYBUTS) { out = readJoyButtons(); return true; }
    if (off >= J_DSP_REGS && off < J_DSP_REGS_END) {
        uint32_t v = dsp.readCtrlReg((off - J_DSP_REGS) & ~3u);
        out = (size == 4) ? v : (size == 2 ? ((off & 2) ? (v & 0xFFFF) : (v >> 16))
                                           : (v >> (8 * (3 - (off & 3)))) & 0xFF);
        return true;
    }
    return false;
}

uint8_t Jaguar::read8(uint32_t a) {
    a &= 0xFFFFFF;
    if ((a & 0xFFC000) == 0xF08000) a -= 0x8000;   // TOM mirror ($F08000-$F0BFFF)
    // 2 MB of DRAM at $000000; $200000-$7FFFFF is empty. (Tempest 2000 issues a
    // blit that runs far past RAM; on hardware those writes go nowhere.)
    if (a < DRAM_SIZE) return dram[a];
    if (a < 0x800000) { if (verbose && getenv("JAG_HOLEREADS")) { static int n = 0; if (n++ < 20) fprintf(stderr, "[hole] read8 $%06X\n", a); } return 0xFF; }
    if (cdUnit && a >= 0xDFFF00 && a < 0xE00000) { uint16_t w = cd.read16(a & 0xFE); return (a & 1) ? (w & 0xFF) : (w >> 8); }
    // Smaller cartridges repeat through the 6 MB cartridge space.
    if (a < 0xE00000) { const auto& rom = cartSpace(); if (rom.empty()) return 0xFF; uint32_t o = (a - ROM_BASE) % rom.size(); return rom[o]; }
    if (a < 0xE20000) {
        uint32_t o = a - BIOS_BASE;
        return o < bios.size() ? bios[o] : 0xFF;
    }
    if (a >= 0xF00000 && a < 0xF04000) {
        uint32_t v; uint32_t off = a - 0xF00000;
        if (tomReadHook(off & ~1u, 2, v)) return (off & 1) ? (v & 0xFF) : (v >> 8);
        return tom[off];
    }
    if (a >= 0xF10000 && a < 0xF20000) {
        uint32_t v; uint32_t off = a - 0xF10000;
        if (off == J_JOYSTICK) return readJoystick() >> 8;   // high byte: no EEPROM side effect
        if (jerryReadHook(off & ~1u, 2, v)) return (off & 1) ? (v & 0xFF) : (v >> 8);
        return jerry[off];
    }
    return 0xFF;
}

uint16_t Jaguar::read16(uint32_t a) {
    a &= 0xFFFFFE;
    if ((a & 0xFFC000) == 0xF08000) a -= 0x8000;   // TOM mirror ($F08000-$F0BFFF)
    if (a < DRAM_SIZE) return be16(&dram[a]);
    if (a < 0x800000) { if (verbose && getenv("JAG_HOLEREADS")) { static int n = 0; if (n++ < 20) fprintf(stderr, "[hole] read16 $%06X\n", a); } return 0xFFFF; }
    if (cdUnit && a >= 0xDFFF00 && a < 0xE00000) return cd.read16(a & 0xFE);
    if (a < 0xE00000) {
        const auto& rom = cartSpace();
        if (rom.size() < 2) return 0xFFFF;
        uint32_t o = (a - ROM_BASE) % rom.size();
        return o + 1 < rom.size() ? be16(&rom[o]) : (uint16_t)(rom[o] << 8 | rom[0]);
    }
    if (a < 0xE20000) {
        uint32_t o = a - BIOS_BASE;
        return o + 1 < bios.size() ? be16(&bios[o]) : 0xFFFF;
    }
    if (a >= 0xF00000 && a < 0xF04000) {
        uint32_t v, off = a - 0xF00000;
        if (tomReadHook(off, 2, v)) return (uint16_t)v;
        return be16(&tom[off]);
    }
    if (a >= 0xF10000 && a < 0xF20000) {
        uint32_t v, off = a - 0xF10000;
        if (jerryReadHook(off, 2, v)) return (uint16_t)v;
        return be16(&jerry[off]);
    }
    return 0xFFFF;
}

uint32_t Jaguar::read32(uint32_t a) {
    a &= 0xFFFFFE;
    if ((a & 0xFFC000) == 0xF08000) a -= 0x8000;   // TOM mirror ($F08000-$F0BFFF)
    if (a < DRAM_SIZE) {
        const uint8_t* p = &dram[a & (DRAM_SIZE - 1)];
        if ((a & (DRAM_SIZE - 1)) <= DRAM_SIZE - 4)
            return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    }
    if (a >= 0xF00000 && a < 0xF04000) {
        uint32_t v, off = a - 0xF00000;
        if (tomReadHook(off, 4, v)) return v;
    } else if (a >= 0xF10000 && a < 0xF20000) {
        uint32_t v, off = a - 0xF10000;
        if (jerryReadHook(off, 4, v)) return v;
    }
    return (uint32_t)read16(a) << 16 | read16(a + 2);
}

void Jaguar::write8(uint32_t a, uint8_t v) {
    a &= 0xFFFFFF;
    if ((a & 0xFFC000) == 0xF08000) a -= 0x8000;   // TOM mirror ($F08000-$F0BFFF)
    if (a < DRAM_SIZE) { TRACE_W(a, v, 1); dram[a] = v; return; }
    if (a < 0x800000) return;   // empty: writes go nowhere
    if (cdUnit && a >= 0xDFFF00 && a < 0xE00000) { cd.write16(a & 0xFE, (a & 1) ? v : (uint16_t)(v << 8)); return; }
    if (a >= 0xF00000 && a < 0xF04000) {
        uint32_t off = a - 0xF00000;
        tom[off] = v;
        tomWriteHook(off & ~1u, tomReg16(off & ~1u), 1);
        return;
    }
    if (a >= 0xF10000 && a < 0xF20000) {
        uint32_t off = a - 0xF10000;
        if (off >= 0xD000 && off < 0xE000) return;   // wavetable ROM
        jerry[off] = v;
        jerryWriteHook(off & ~1u, jerryReg16(off & ~1u), 1);
        return;
    }
}

void Jaguar::write16(uint32_t a, uint16_t v) {
    a &= 0xFFFFFE;
    if ((a & 0xFFC000) == 0xF08000) a -= 0x8000;   // TOM mirror ($F08000-$F0BFFF)
    if (a < 0x800000 && a >= DRAM_SIZE) return;   // empty
    if (cdUnit && a >= 0xDFFF00 && a < 0xE00000) { cd.write16(a & 0xFE, v); return; }
    if (a < DRAM_SIZE) { TRACE_W(a, v, 2); uint8_t* p = &dram[a]; p[0] = v >> 8; p[1] = v & 0xFF; return; }
    if (a >= 0xF00000 && a < 0xF04000) {
        uint32_t off = a - 0xF00000;
        tom[off] = v >> 8; tom[off + 1] = v & 0xFF;
        tomWriteHook(off, v, 2);
        return;
    }
    if (a >= 0xF10000 && a < 0xF20000) {
        uint32_t off = a - 0xF10000;
        if (off >= 0xD000 && off < 0xE000) return;   // wavetable ROM
        jerry[off] = v >> 8; jerry[off + 1] = v & 0xFF;
        jerryWriteHook(off, v, 2);
        return;
    }
}

void Jaguar::write32(uint32_t a, uint32_t v) {
    a &= 0xFFFFFE;
    if ((a & 0xFFC000) == 0xF08000) a -= 0x8000;   // TOM mirror ($F08000-$F0BFFF)
    if (a >= DRAM_SIZE && a < 0x800000) return;   // empty
    if (a < DRAM_SIZE) {
        uint32_t o = a & (DRAM_SIZE - 1);
        TRACE_W(a, v, 4);
        if (o <= DRAM_SIZE - 4) {
            uint8_t* p = &dram[o];
            p[0] = v >> 24; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF;
            return;
        }
    }
    if (a >= 0xF00000 && a < 0xF04000) {
        uint32_t off = a - 0xF00000;
        tom[off] = v >> 24; tom[off+1] = (v >> 16) & 0xFF; tom[off+2] = (v >> 8) & 0xFF; tom[off+3] = v & 0xFF;
        tomWriteHook(off, v, 4);
        return;
    }
    if (a >= 0xF10000 && a < 0xF20000) {
        uint32_t off = a - 0xF10000;
        if (off >= 0xD000 && off < 0xE000) return;   // wavetable ROM
        jerry[off] = v >> 24; jerry[off+1] = (v >> 16) & 0xFF; jerry[off+2] = (v >> 8) & 0xFF; jerry[off+3] = v & 0xFF;
        jerryWriteHook(off, v, 4);
        return;
    }
    write16(a, v >> 16);
    write16(a + 2, v & 0xFFFF);
}

// A 32-bit register is "complete" after a long write or a write to its low word.
static inline bool completes32(uint32_t off, int size) { return size == 4 || (off & 2); }

#ifdef JAG_TRACE
extern "C" void jag_trace_tomwrite(unsigned off, unsigned value, int size);
#endif
void Jaguar::tomWriteHook(uint32_t off, uint32_t value, int size) {
    if (verbose && getenv("JAG_TW") && off >= (uint32_t)strtoul(getenv("JAG_TW"), nullptr, 16) && off < (uint32_t)strtoul(getenv("JAG_TW"), nullptr, 16) + 4) fprintf(stderr, "[tw] f%ld vc=%d master=%d off=%X val=%X size=%d ppc=%06X\n", (long)frameCount, halfLine, busMaster, off, value, size, (unsigned)m68k_get_reg(nullptr, M68K_REG_PPC));
    if (verbose && getenv("JAG_BGWATCH") && off == 0x58 && (long)frameCount == atol(getenv("JAG_BGWATCH"))) fprintf(stderr, "[bgw] vc=%d val=%04X r16=%08X/%08X r10=%08X/%08X r18=%08X/%08X\n", halfLine, value & 0xFFFF, gpu.regs[16], gpu.regs[48], gpu.regs[10], gpu.regs[42], gpu.regs[18], gpu.regs[50]);
#ifdef JAG_TRACE
    jag_trace_tomwrite(off, value, size);
#endif
    // Writing the TOM PIT registers (re)starts its count: games re-arm the timer
    // each frame to time a raster interrupt.
    if (off < 0x54 && off + (uint32_t)size > T_PIT0) tomPitAcc = 0;
    // Blitter B_I0..B_I3 ($F0227C-$F0228B) are aliases into the pattern and
    // source data registers: byte 1 of each is that lane's integer intensity
    // (in PATD) and bytes 2-3 its fraction (in SRCD). $F0227C is the rightmost lane.
    for (uint32_t b = off; b < off + (uint32_t)size; b++) {
        if (b < 0x227C || b >= 0x228C) continue;
        const uint32_t L = (b - 0x227C) / 4, k = (b - 0x227C) % 4;
        if (k == 1) tom[0x2268 + 7 - 2 * L] = tom[b];
        else if (k == 2) tom[0x2240 + 6 - 2 * L] = tom[b];
        else if (k == 3) tom[0x2240 + 7 - 2 * L] = tom[b];
    }
    if (off == T_INT1 && size != 4) {
        tomIntEnable = value & 0x1F;
        tomIntPending &= ~((value >> 8) & 0x1F);
        updateM68kIrq();
        return;
    }
    if (size == 4 && off == T_INT1) {  // long write covering INT1/INT2
        uint16_t w = value >> 16;
        tomIntEnable = w & 0x1F;
        tomIntPending &= ~((w >> 8) & 0x1F);
        updateM68kIrq();
        return;
    }
    if (off >= T_GPU_REGS && off < T_GPU_REGS_END) {
        if (completes32(off, size)) {
            uint32_t base = off & ~3u;
            gpu.writeCtrlReg(base - T_GPU_REGS, tomReg32(base));
        }
        return;
    }
    if ((off & ~3u) == T_BLIT_CMD && completes32(off, size)) {
        blitterGo();
        return;
    }
}

void Jaguar::jerryWriteHook(uint32_t off, uint32_t value, int size) {
    if (verbose && getenv("JAG_SCLKLOG") && (off & ~3u) >= J_LTXD + 8 && (off & ~3u) <= J_SMODE)
        fprintf(stderr, "[jerry] write %s $F1%04X = %08X size %d frame %llu\n", (off & ~3u) == J_SCLK ? "SCLK" : "SMODE", off, value, size, (unsigned long long)frameCount);
    if (off == J_INT) {
        uint16_t w = (size == 4) ? (value >> 16) : value;
        jerryIntEnable = w & 0x3F;
        jerryIntPending &= ~((w >> 8) & 0x3F);
        return;
    }
    if (off == J_JOYSTICK) {
        if (verbose && getenv("JAG_JOYLOG") && (long)frameCount == atol(getenv("JAG_JOYLOG"))) { static int n = 0; if (n++ < 40) fprintf(stderr, "[joy] write %08X size %d by %d\n", value, size, busMaster); }
        joySelect = (size == 4) ? (value >> 16) : value; return;
    }
    if ((off == 0x4800 || off == 0x5000) && verbose && getenv("JAG_EELOG")) { static long n = 0; if (n++ < 300) fprintf(stderr, "[cart-ee] frame %llu off=%04X val=%X size=%d\n", (unsigned long long)frameCount, off, value, size); }
    if (off == 0x4800) { eeSetDI((int)((size == 4) ? (value >> 16) : value) & 1); return; }   // EEPROM data in (clocks a bit)
    if (off == 0x5000) { eeSetCS(); return; }                                                 // EEPROM chip select
    if (off >= J_DSP_REGS && off < J_DSP_REGS_END) {
        if (completes32(off, size)) {
            uint32_t base = off & ~3u;
            uint32_t v = (uint32_t)jerryReg16(base) << 16 | jerryReg16(base + 2);
            dsp.writeCtrlReg(base - J_DSP_REGS, v);
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Joypad matrix (port 1). Rows are selected by writing a low bit in JOYSTICK
// bits 0-3; the row's four lines appear in bits 8-11 and its button line in
// JOYBUTS bit 1 (row 0 also has PAUSE on bit 0). All active low.
// ---------------------------------------------------------------------------
uint16_t Jaguar::readJoystick() {
    uint16_t sel = joySelect & 0x0F;
    uint16_t lines = 0x0F;
    auto pressed = [&](uint32_t b) { return (joyButtons & b) != 0; };
    if (!(sel & 1)) {
        if (pressed(JB_UP)) lines &= ~1; if (pressed(JB_DOWN)) lines &= ~2;
        if (pressed(JB_LEFT)) lines &= ~4; if (pressed(JB_RIGHT)) lines &= ~8;
    } else if (!(sel & 2)) {
        if (pressed(JB_STAR)) lines &= ~1; if (pressed(JB_7)) lines &= ~2;
        if (pressed(JB_4)) lines &= ~4; if (pressed(JB_1)) lines &= ~8;
    } else if (!(sel & 4)) {
        if (pressed(JB_0)) lines &= ~1; if (pressed(JB_8)) lines &= ~2;
        if (pressed(JB_5)) lines &= ~4; if (pressed(JB_2)) lines &= ~8;
    } else if (!(sel & 8)) {
        if (pressed(JB_HASH)) lines &= ~1; if (pressed(JB_9)) lines &= ~2;
        if (pressed(JB_6)) lines &= ~4; if (pressed(JB_3)) lines &= ~8;
    }
    return (uint16_t)(0xF000 | (lines << 8) | 0x00FF);
}

uint16_t Jaguar::readJoyButtons() {
    uint16_t sel = joySelect & 0x0F;
    uint16_t b = 0x0F;
    auto pressed = [&](uint32_t m) { return (joyButtons & m) != 0; };
    if (!(sel & 1)) { if (pressed(JB_PAUSE)) b &= ~1; if (pressed(JB_A)) b &= ~2; }
    else if (!(sel & 2)) { if (pressed(JB_B)) b &= ~2; }
    else if (!(sel & 4)) { if (pressed(JB_C)) b &= ~2; }
    else if (!(sel & 8)) { if (pressed(JB_OPTION)) b &= ~2; }
    return (uint16_t)(0xFFE0 | (pal ? 0 : 0x10) /* region: bit 4 set = NTSC */ | b);
}

// ---------------------------------------------------------------------------
// Timers and audio clock
// ---------------------------------------------------------------------------
void Jaguar::tickTimers(double sys) {
    if (blitBusy > 0) {
        blitBusy -= sys;
        if (blitBusy <= 0) blitBusy = 0;
    }
    if (blitBusy <= 0 && blitIntPending) { blitIntPending = false; gpu.raiseInt(4); }
    // TOM PIT -> 68k (bit 3) and GPU interrupt 2
    {
        uint32_t pre = tomReg16(T_PIT0), div = tomReg16(T_PIT1);
        if (pre) {   // a zero divider just means divide by 1
            double period = (double)(pre + 1) * (div + 1);
            tomPitAcc += sys;
            while (tomPitAcc >= period) { tomPitAcc -= period; tomRaise(3); gpu.raiseInt(2); }
        }
    }
    // JERRY timer 1 and 2
    {
        // A JERRY timer runs when either value is non-zero; zero means divide
        // by 1. Pinball Fantasies sets prescaler $25A with divider 0 for its
        // ~44 kHz sample timer, which we left stopped (no sound at all).
        uint32_t pre = jerryReg16(J_PIT1), div = jerryReg16(J_PIT2);
        if (pre || div) {
            double period = (double)(pre + 1) * (div + 1);
            jPit1Acc += sys;
            while (jPit1Acc >= period) { jPit1Acc -= period; dsp.raiseInt(2); jerryRaise(2); }
        }
        pre = jerryReg16(J_PIT3); div = jerryReg16(J_PIT4);
        if (pre || div) {
            double period = (double)(pre + 1) * (div + 1);
            jPit2Acc += sys;
            while (jPit2Acc >= period) { jPit2Acc -= period; dsp.raiseInt(3); jerryRaise(3); }
        }
    }
    // I2S: one stereo sample per period; DSP interrupt 1 asks for the next one.
    if (!i2sExternal()) {
        double period = clock() / audioRate();
        i2sAcc += sys;
        while (i2sAcc >= period) {
            i2sAcc -= period;
            int16_t l = (int16_t)jerryReg16(J_LTXD + 2);
            int16_t r = (int16_t)jerryReg16(J_RTXD + 2);
            if (verbose && getenv("JAG_AUDLOG") && (long)frameCount == atol(getenv("JAG_AUDLOG"))) {
                static int n = 0;
                if (n++ < 60) fprintf(stderr, "[aud] L=%04X%04X R=%04X%04X smode=%04X\n", jerryReg16(J_LTXD), jerryReg16(J_LTXD + 2),
                                      jerryReg16(J_RTXD), jerryReg16(J_RTXD + 2), jerryReg16(J_SMODE + 2));
            }
            audio.push_back(l);
            audio.push_back(r);
            if (dsp.running) dsp.raiseInt(1);
        }
    }
}

// ---------------------------------------------------------------------------
// Frame scheduler: per scanline, interleave 68000 / GPU / DSP in slices.
// ---------------------------------------------------------------------------
void Jaguar::runFrame() {
    audio.clear();
    const int lines = linesPerFrame();
    const double sysPerLine = clock() / (refreshHz() * lines);
    const int SLICES = 8;
    const int m68kPerSlice = (int)(sysPerLine / 2.0 / SLICES);
    const int riscPerSlice = (int)(sysPerLine / SLICES);

    uint16_t vdb = tomReg16(T_VDB), vde = tomReg16(T_VDE);
    // The object processor runs on every line from VDB to VDE, including lines
    // in vertical blank that are never shown; games put GPU objects there.
    const bool opEveryLine = vde <= vdb || vde > 2 * lines;   // VDE=$FFFF: "run the OP on every line"
    int opEnd = opEveryLine ? 2 * lines : vde;
    if (vde <= vdb || vde > 2 * lines) vde = (uint16_t)(2 * lines - 4);
    uint16_t vbb = tomReg16(T_VBB);
    if (vbb > vdb && vbb < vde) vde = vbb;   // nothing is shown once vertical blank starts
    int firstVisible = vdb & ~1;
    int maxVisible = std::min(pal ? 288 : 240, (vde - firstVisible) / 2);
    int pwidth = ((tomReg16(T_VMODE) >> 9) & 7) + 1;
    // The output grid uses the narrowest pixel width seen last frame, so lines shown
    // with wider pixels (PWIDTH changed mid-frame) are stretched instead of squeezed.
    if (minPwSeen <= 8) pwidth = std::min(pwidth, minPwSeen);
    basePw = pwidth; minPwSeen = 99;
    // Visible width comes from the display window HDB1..HDE (in video clocks;
    // bit 10 marks the second half of the line) divided by the pixel width.
    {
        int hp = tomReg16(T_HP) & 0x3FF; if (hp < 100) hp = 844;
        auto hpos = [&](uint16_t v) { return (v & 0x400) ? hp + (v & 0x3FF) : (v & 0x3FF); };
        int span = hpos(tomReg16(T_HDE)) - hpos(tomReg16(T_HDB1));
        int w = (span > 64 * pwidth) ? span / pwidth : (int)lrint(1280.0 / pwidth);
        fbW = std::max(64, std::min<int>(MAX_W, w));
    }
    fbH = std::max(1, maxVisible);

    for (int line = 0; line < lines; line++) {
        halfLine = line * 2;
        uint16_t vi = tomReg16(T_VI);
        if (vi >= halfLine && vi < halfLine + 2) tomRaise(0);

        bool visible = halfLine >= firstVisible && (halfLine - firstVisible) / 2 < maxVisible;
        {   // The OP walks the list from the top of the frame, not from VDB: GPU
            // objects (and branches) fire on every line, only bitmaps wait for the
            // display window. Super Burnout counts these per-line GPU interrupts to
            // step its sky/ground colour table and expects entry N at about line N
            // from the frame start; starting at VDB left a pale strip (and a late
            // ground colour) at the horizon.
            static int pre = getenv("JAG_OPPRE") ? atoi(getenv("JAG_OPPRE")) : 0;
            if (pre >= 0 && halfLine >= pre && halfLine < (vdb & ~1)) {
                opSkipBitmaps = true; busMaster = 4; opRenderLine(halfLine); busMaster = 0; opSkipBitmaps = false;
            }
        }
        if (halfLine >= (vdb & ~1) && halfLine < opEnd) {
            // With VDE=$FFFF, bitmap objects stop at the bottom of the shown
            // picture; below it the list is still walked so GPU objects and
            // branches fire, but bitmaps are not drawn or written back. Super
            // Burnout swaps in the next frame's list at line 520, and passes
            // after that ate the top row of every piece of text.
            opSkipBitmaps = opEveryLine && halfLine >= firstVisible + 2 * maxVisible;
            busMaster = 4; opRenderLine(halfLine); busMaster = 0; opSkipBitmaps = false;
            if (visible) outputLine(halfLine);
            else if (tomReg16(T_VMODE) & 0x80) {   // still clear the line buffer like a shown line
                std::fill(lineBuf, lineBuf + 720, tomReg16(T_BG));
                std::fill(lineBuf32, lineBuf32 + 720, 0u);
                if (shadeTag) std::fill(lineFrac, lineFrac + 720, (uint16_t)0);
            }
        }
        for (int s = 0; s < SLICES; s++) {
            lineSlice = s;
            // VC counts half-lines: odd for the second half of each line.
            halfLine = line * 2 + (s >= SLICES / 2 ? 1 : 0);
            // Bus priority: the blitter, GPU and DSP all outrank the 68000, so it only
            // gets the bus time they leave over. The RISCs run first so their memory
            // traffic in this slice is known.
            {
                const double sliceSys = sysPerLine / SLICES;
                riscBusUse = 0;
                busMaster = 1; gpu.run(riscPerSlice);
                busMaster = 2; dsp.run(riscPerSlice);
                // The blitter shares the bus with the 68000 in page-mode bursts; it never
                // shuts the CPU out entirely (Fight for Life's 2 ms blits froze it long
                // enough to take its video interrupt tens of lines late).
                static const double blitShare = getenv("JAG_BLITSHARE") ? atof(getenv("JAG_BLITSHARE")) : 0.75;
                // Only while an interrupt is pending or being serviced: otherwise keep the
                // blitter's full hold (Iron Soldier's mission start depends on the 68000
                // being starved while its blits run).
                const bool irqBusy = (tomIntPending & tomIntEnable & 0x1F) != 0 || (m68k_get_reg(nullptr, M68K_REG_SR) & 0x0700) != 0;
                static const bool shareAlways = getenv("JAG_SHAREALWAYS") != nullptr;
            const double share = (irqBusy || shareAlways) ? blitShare : 1.0;
                double held = std::min(sliceSys, std::min(std::min(blitBusy, sliceSys), sliceSys * share) + riscBusUse);
                int cyc = (int)(m68kPerSlice * (1.0 - held / sliceSys));
                { static const char* hl = getenv("JAG_HELDLOG");
                  if (hl && (long)frameCount == atol(hl) && s == 0 && line < 120)
                      fprintf(stderr, "[held] vc=%d blit=%.0f risc=%.0f cyc=%d/%d gpu=%d dsp=%d\n", halfLine, blitBusy, riscBusUse, cyc, (int)m68kPerSlice, (int)gpu.running, (int)dsp.running); }
                busMaster = 0; if (cyc > 0) m68k_execute(cyc);
            }
            busMaster = 0;
            tickTimers(sysPerLine / SLICES);
            if (cdUnit) cd.tick(sysPerLine / SLICES);
        }
    }
    frameCount++;
}

} // namespace jag

// ---------------------------------------------------------------------------
// Musashi callbacks
// ---------------------------------------------------------------------------
extern "C" {
using jag::g_jag;
unsigned int m68k_read_memory_8(unsigned int a) { return g_jag->read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return g_jag->read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return g_jag->read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { g_jag->write8(a, (uint8_t)v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { g_jag->write16(a, (uint16_t)v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { g_jag->write32(a, v); }
unsigned int m68k_read_disassembler_8(unsigned int a) { return g_jag->read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return g_jag->read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return g_jag->read32(a); }
// TOM presents vector 64 ($100) for its level-2 interrupt.
int jag_m68k_int_ack(int level) {
    static const char* il = getenv("JAG_INTLOG");
    if (il && g_jag && (long)g_jag->frameCount >= atol(il) && (long)g_jag->frameCount < atol(il) + 8)
        fprintf(stderr, "[intack] level %d frame %llu VC=%d slice=%d pend=%02X\n", level, (unsigned long long)g_jag->frameCount, g_jag->halfLine, g_jag->lineSlice, g_jag->tomIntPending);
    return level == 2 ? 64 : (int)M68K_INT_ACK_AUTOVECTOR; }
}

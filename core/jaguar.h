// JagDroid - an original Atari Jaguar emulator core.
// Written from public hardware documentation. The 68000 core is Musashi (MIT).
#pragma once
#include <cstdint>
#include <memory>
#include <cstring>
#include <string>
#include <vector>
#include "cdrom.h"

namespace jag {

class Jaguar;

// ---------------------------------------------------------------------------
// GPU (in TOM) and DSP (in JERRY) share one RISC design with small differences.
// ---------------------------------------------------------------------------
extern bool pcWatchOn;
struct Risc {
    enum Flag : uint32_t {
        ZERO = 1u << 0, CARRY = 1u << 1, NEG = 1u << 2, IMASK = 1u << 3, REGPAGE = 1u << 14,
    };

    Jaguar* jag = nullptr;
    bool isDsp = false;
    const char* name = "GPU";

    uint32_t regs[64] = {};      // two banks of 32
    uint32_t pc = 0;
    uint32_t flags = 0;
    uint32_t intEnable = 0;      // bit n enables interrupt n
    uint32_t intLatch = 0;       // bit n pending for interrupt n
    uint32_t mtxc = 0, mtxa = 0, endian = 0, hidata = 0, remain = 0, divctrl = 0;
    uint32_t modMask = 0xFFFFFFFF; // DSP D_MOD
    int64_t mac = 0;
    bool running = false;
    bool singleStep = false;     // CTRL bit 3: halt after each instruction until SINGLE_GO (bit 4)
    int stepGrant = 0;           // instructions allowed while single-stepping
    bool cpuIntRequest = false;  // CTRL bit 1: interrupt the 68000

    // Local RAM (4 KB GPU at $F03000, 8 KB DSP at $F1B000)
    uint32_t ramBase = 0, ramSize = 0;
    uint8_t* ram = nullptr;

    // Delayed branch state
    bool branchPending = false;
    uint32_t branchTarget = 0;

    int64_t cycleDebt = 0;
    uint64_t instrCount = 0;
    int traceLeft = 0;
    int lastDest = -1;       // register written by the previous instruction (interlock)   // diagnostics: print this many instructions

    void reset();
    void run(int cycles);
    void raiseInt(int n);
    void extAccess(bool write, uint32_t addr);
    void pcWatch(uint32_t addr);
    void checkInterrupts();

    uint32_t readCtrlReg(uint32_t off);
    void writeCtrlReg(uint32_t off, uint32_t v);

private:
    // A FLAGS write changes the register bank only after the following
    // instruction has read its operands: interrupt handlers end with
    // "store flags; jump (r3)" and the jump still takes r3 from bank 0.
    int bankHold = 0; uint32_t heldBank = 0; bool inDelaySlot = false;
    inline uint32_t bank() const { return bankHold > 0 ? heldBank : ((flags & IMASK) ? 0 : ((flags & REGPAGE) ? 32 : 0)); }
    inline uint32_t& R(int n) { return regs[bank() + n]; }
    inline uint32_t& ALT(int n) { return regs[(bank() ^ 32) + n]; }
    uint16_t fetch16(uint32_t addr);
    uint32_t load32(uint32_t addr);
    void store32(uint32_t addr, uint32_t v);
    void setZN(uint32_t v) {
        flags &= ~(ZERO | NEG);
        if (v == 0) flags |= ZERO;
        if (v & 0x80000000u) flags |= NEG;
    }
    void setC(bool c) { if (c) flags |= CARRY; else flags &= ~CARRY; }
    bool condition(int cc) const;
    void step();
};

// ---------------------------------------------------------------------------
// Joypad state (one controller, port 1)
// ---------------------------------------------------------------------------
enum JoyButton : uint32_t {
    JB_UP = 1u << 0, JB_DOWN = 1u << 1, JB_LEFT = 1u << 2, JB_RIGHT = 1u << 3,
    JB_A = 1u << 4, JB_B = 1u << 5, JB_C = 1u << 6, JB_PAUSE = 1u << 7, JB_OPTION = 1u << 8,
    JB_0 = 1u << 9, JB_1 = 1u << 10, JB_2 = 1u << 11, JB_3 = 1u << 12, JB_4 = 1u << 13,
    JB_5 = 1u << 14, JB_6 = 1u << 15, JB_7 = 1u << 16, JB_8 = 1u << 17, JB_9 = 1u << 18,
    JB_STAR = 1u << 19, JB_HASH = 1u << 20,
};

// ---------------------------------------------------------------------------
// The whole machine
// ---------------------------------------------------------------------------
class Jaguar {
public:
    static constexpr uint32_t DRAM_SIZE = 0x200000;   // 2 MB
    static constexpr uint32_t ROM_BASE = 0x800000;
    static constexpr uint32_t ROM_MAX = 0x600000;     // up to 6 MB
    static constexpr uint32_t BIOS_BASE = 0xE00000;
    static constexpr uint32_t BIOS_SIZE = 0x20000;    // 128 KB

    static constexpr int MAX_W = 720, MAX_H = 576;

    Jaguar();
    ~Jaguar();

    bool loadCart(const std::vector<uint8_t>& data, std::string* err = nullptr);
    bool loadBios(const std::vector<uint8_t>& data, std::string* err = nullptr);
    void reset();
    void runFrame();

    // Output
    const uint32_t* framebuffer() const { return fb; }   // RGBA8888 (R in low byte)
    int frameWidth() const { return fbW; }
    int frameHeight() const { return fbH; }
    // Audio: interleaved stereo int16 samples produced during the last frame
    std::vector<int16_t> audio;
    int audioRate() const;

    void setButtons(uint32_t mask) { joyButtons = mask; }

    // Cartridge serial EEPROM (93C46: 64 x 16-bit words) used for game saves.
    // The host loads it before reset and writes it out when eepromDirty is set.
    uint16_t eeprom[64];
    bool eepromDirty = false;
    uint32_t rngState = 0x2545F491u;   // JERRY random number generator ($F1003A)
    void setEeprom(const uint8_t* data, size_t len);   // big-endian bytes, up to 128
    void getEeprom(uint8_t* out128) const;
    // The CD unit's own save chip (what CD games and the BIOS keep their saves in).
    void setMemoryTrack(const uint8_t* data, size_t len) { cd.eeprom.load(data, len); }
    void getMemoryTrack(uint8_t* out128) const { cd.eeprom.store(out128); }
    bool memoryTrackDirty() const { return cd.eeprom.dirty; }
    void clearMemoryTrackDirty() { cd.eeprom.dirty = false; }

    // Jaguar CD: with a disc inserted the CD BIOS runs from the cartridge slot
    // and the Butch controller appears at $DFFF00.
    std::unique_ptr<CdImage> disc;
    Butch cd;
    bool cdUnit = false;
    uint16_t i2sRxL = 0, i2sRxR = 0, i2sPrevR = 0;
    bool i2sExternal() const;
    void cdI2sFrame(uint16_t w0, uint16_t w1);
    bool insertDisc(const std::string& cuePath, std::string* err = nullptr);
    void insertDisc(std::unique_ptr<CdImage> image);

    // Save states: the whole machine except the cartridge and BIOS images.
    std::vector<uint8_t> saveState() const;
    bool loadState(const std::vector<uint8_t>& st, std::string* err = nullptr);

    // Debug / test access
    uint8_t read8(uint32_t a);
    uint16_t read16(uint32_t a);
    uint32_t read32(uint32_t a);
    void write8(uint32_t a, uint8_t v);
    void write16(uint32_t a, uint16_t v);
    void write32(uint32_t a, uint32_t v);
    uint64_t frameCount = 0;
    int busMaster = 0;   // 0 68k, 1 GPU, 2 DSP, 3 blitter, 4 OP (diagnostics)
    bool verbose = false;

    // --- subsystems (public for the components' convenience) ---
    uint8_t* dram;
    std::vector<uint8_t> rom;
    std::vector<uint8_t> topCart;   // cartridge plugged into the CD unit's pass-through slot
    // With a CD unit, cartridge space shows the CD BIOS while BUTCH bit 18 is set,
    // otherwise whatever cartridge sits on top of the unit.
    const std::vector<uint8_t>& cartSpace() const { return (cdUnit && !(cd.ctrl & 0x40000)) ? topCart : rom; }
    std::vector<uint8_t> bios;
    bool hasBios = false;
    uint8_t tom[0x4000];      // $F00000-$F03FFF: registers, CLUT, line buffers, GPU RAM
    uint8_t jerry[0x10000];   // $F10000-$F1FFFF: registers, DSP RAM, wave tables

    Risc gpu, dsp;

    // TOM interrupt state (INT1)
    uint16_t tomIntEnable = 0, tomIntPending = 0;
    void tomRaise(int bit);
    // JERRY interrupt state (J_INT)
    uint16_t jerryIntEnable = 0, jerryIntPending = 0;
    void jerryRaise(int bit);
    void updateM68kIrq();

    // Timing
    static constexpr double CLOCK_NTSC = 26590906.0;   // system clock (GPU/DSP/blitter)
    static constexpr double CLOCK_PAL = 26593900.0;
    bool pal = false;          // PAL console: 50 Hz, region bit clear; set before reset()
    double clock() const { return pal ? CLOCK_PAL : CLOCK_NTSC; }
    double refreshHz() const { return pal ? 50.0 : 59.94; }
    int halfLine = 0;          // current VC
    int lineSlice = 0;         // which slice of the current line is running (drives HC)
    uint16_t hcount() const;
    int linesPerFrame() const;

    // Blitter
    void blitterGo();
    uint64_t blitCount = 0;
    // The blitter's memory traffic takes time: it reports busy, holds the bus
    // away from the 68000 and raises its interrupt only once that time has passed.
    double blitBusy = 0;           // system cycles of blitter work still outstanding
    bool blitIntPending = false;
    bool opSkipBitmaps = false;     // OP pass below the shown picture: walk the list, skip bitmaps
    double riscBusUse = 0;         // bus clocks the GPU/DSP used this slice (the 68000 waits for them)

    // Object processor & video
    void opRenderLine(int vc);
    void outputLine(int vc);

    uint32_t olpCurrent = 0;
    uint16_t lineBuf[1440];      // working line buffer (16-bit pixels)
    uint32_t lineBuf32[720];     // RGB24 mode
    bool opRunning = false;
    uint16_t* rawOut = nullptr;   // diagnostics: raw 16-bit pixels of the last frame (MAX_W stride)

private:
    uint32_t* fb;
    int fbW = 320, fbH = 240;
public:
    int basePw = 4, minPwSeen = 8;
    mutable uint32_t hcKey = 0xFFFFFFFF; mutable int hcReads = 0;   // see hcount()   // games may change PWIDTH mid-frame (Doom: 160-wide view, 320-wide status bar)
    uint32_t cryTable[65536];
    void buildCryTable();

    // ---- Display enhancements (off by default; they never change emulation) ----
    // With shading tracking on, every 16-bit pixel the blitter Gouraud-shades
    // remembers the 8 bits of intensity below the 8 the hardware keeps, tagged
    // with the value written so any later write by anything else voids it.
    // The object processor carries that through to the frame, and renderSmooth
    // builds a picture from it: shaded surfaces get their full precision
    // (dithered to 8 bits) and, at 2x, are interpolated between pixels while
    // edges, textures and 2D art stay sharp.
    enum : uint16_t { SH_FRAC = 0xFF, SH_SHADED = 0x100, SH_SMOOTH = 0x200, SH_CRY = 0x400 };
    void setShadeTracking(bool on);
    bool shadeTracking() const { return shadeTag != nullptr; }
    void renderSmooth(int scale, uint32_t* out) const;   // fbW*scale x fbH*scale, packed rows
    uint32_t* shadeTag = nullptr;       // per DRAM word: value << 16 | info
    uint32_t* fbInfo = nullptr;         // per frame pixel: CRY value << 16 | info
    uint16_t lineFrac[1440];
    void shadeNote(uint32_t addr, uint16_t value, uint16_t info) {
        if (addr < DRAM_SIZE) shadeTag[addr >> 1] = (uint32_t)value << 16 | info;
    }
    uint16_t shadeInfo(uint32_t addr) const {
        if (!shadeTag || addr >= DRAM_SIZE - 1) return 0;
        uint32_t t = shadeTag[addr >> 1];
        return (t >> 16) == ((uint32_t)dram[addr & ~1u] << 8 | dram[addr | 1]) ? (uint16_t)t : 0;
    }

    uint32_t joyButtons = 0;
    uint16_t joySelect = 0xFFFF;

    // timers
    double tomPitAcc = 0, jPit1Acc = 0, jPit2Acc = 0, i2sAcc = 0;
    void tickTimers(double sysCycles);

    void bootWithoutBios();

    // register helpers
    uint16_t tomReg16(uint32_t off) const { return (uint16_t)((tom[off] << 8) | tom[off + 1]); }
    uint16_t jerryReg16(uint32_t off) const { return (uint16_t)((jerry[off] << 8) | jerry[off + 1]); }
    uint32_t tomReg32(uint32_t off) const { return ((uint32_t)tomReg16(off) << 16) | tomReg16(off + 2); }

    void tomWriteHook(uint32_t off, uint32_t value, int size);
    void jerryWriteHook(uint32_t off, uint32_t value, int size);
    bool tomReadHook(uint32_t off, int size, uint32_t& out);
    bool jerryReadHook(uint32_t off, int size, uint32_t& out);
    uint16_t readJoystick();
    // EEPROM serial interface: DI at $F14800, chip select at $F15000, DO in bit 0 of $F14000
    int eeState = 0, eeOp = 0, eeBits = 0, eeAddr = 0, eeOutBits = 0;
    uint32_t eeShift = 0;
    bool eeWriteEnable = false;
    void eeSetCS();
    void eeSetDI(int bit);
    int eeGetDO();
    uint32_t romChecksum = 0;
    uint16_t readJoyButtons();

    friend struct Blitter;
};

extern Jaguar* g_jag;   // used by Musashi callbacks

} // namespace jag

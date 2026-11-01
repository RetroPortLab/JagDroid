// Game saves (cartridge EEPROM) and emulator save states.
//
// Jaguar cartridges that save carry a 93C46 serial EEPROM: 64 words of 16 bits,
// driven bit by bit through JERRY. Writing $F15000 selects the chip (and starts a
// new command), each write to $F14800 clocks one bit in from bit 0, and the chip's
// output appears in bit 0 of $F14000, advancing one bit per read. Commands are a
// start bit, a 2-bit opcode and a 6-bit address:
//   10 READ   -> a dummy 0 is skipped by games; 16 data bits follow (MSB first)
//   01 WRITE  -> 16 data bits follow; the chip then reports busy once
//   11 ERASE  -> word becomes $FFFF
//   00 + address bits 5-4: 11 write enable, 00 write disable, 10 erase all, 01 write all
#include "jaguar.h"
#include <algorithm>
#include <type_traits>
#include <cstring>

extern "C" {
#include "m68kcpu.h"
}

namespace jag {

enum { EE_IDLE, EE_OPCODE, EE_ADDR, EE_READ, EE_WRITEDATA, EE_WRALDATA, EE_BUSY };

void Jaguar::setEeprom(const uint8_t* data, size_t len) {
    for (int i = 0; i < 64; i++) eeprom[i] = 0xFFFF;   // a blank chip reads as all ones
    for (size_t i = 0; i + 1 < len && i < 128; i += 2) eeprom[i / 2] = (uint16_t)(data[i] << 8 | data[i + 1]);
    eepromDirty = false;
}

void Jaguar::getEeprom(uint8_t* out) const {
    for (int i = 0; i < 64; i++) { out[i * 2] = eeprom[i] >> 8; out[i * 2 + 1] = eeprom[i] & 0xFF; }
}

void Jaguar::eeSetCS() {
    if (eeState != EE_BUSY) eeState = EE_IDLE;
    eeBits = 0; eeShift = 0;
}

void Jaguar::eeSetDI(int bit) {
    bit &= 1;
    switch (eeState) {
    case EE_IDLE:
    case EE_BUSY:
        // Every command begins with a start bit; the first clock is taken as it.
        eeState = EE_OPCODE; eeBits = 0; eeOp = 0;
        break;
    case EE_OPCODE:
        eeOp = (eeOp << 1) | bit;
        if (++eeBits == 2) { eeState = EE_ADDR; eeBits = 0; eeAddr = 0; }
        break;
    case EE_ADDR:
        eeAddr = (eeAddr << 1) | bit;
        if (++eeBits == 6) {
            eeBits = 0; eeShift = 0;
            switch (eeOp) {
            case 2: eeState = EE_READ; eeOutBits = 16; break;          // READ
            case 1: eeState = EE_WRITEDATA; break;                       // WRITE
            case 3:                                                      // ERASE
                if (eeWriteEnable) { eeprom[eeAddr] = 0xFFFF; eepromDirty = true; }
                eeState = EE_BUSY; break;
            default:
                switch (eeAddr >> 4) {
                case 3: eeWriteEnable = true; eeState = EE_IDLE; break;   // EWEN
                case 0: eeWriteEnable = false; eeState = EE_IDLE; break;  // EWDS
                case 2:                                                   // ERAL
                    if (eeWriteEnable) { for (auto& w : eeprom) w = 0xFFFF; eepromDirty = true; }
                    eeState = EE_BUSY; break;
                case 1: eeState = EE_WRALDATA; break;                     // WRAL
                }
            }
        }
        break;
    case EE_WRITEDATA:
    case EE_WRALDATA:
        eeShift = (eeShift << 1) | bit;
        if (++eeBits == 16) {
            if (eeWriteEnable) {
                if (eeState == EE_WRITEDATA) eeprom[eeAddr] = (uint16_t)eeShift;
                else for (auto& w : eeprom) w = (uint16_t)eeShift;
                eepromDirty = true;
            }
            eeState = EE_BUSY;
        }
        break;
    case EE_READ:
        break;   // clocks during a read just move the output on (see eeGetDO)
    }
}

int Jaguar::eeGetDO() {
    switch (eeState) {
    case EE_READ: {
        eeOutBits--;
        int bit = (eeprom[eeAddr] >> eeOutBits) & 1;
        if (eeOutBits == 0) eeState = EE_IDLE;
        return bit;
    }
    case EE_BUSY:
        eeState = EE_IDLE;   // report busy once, then ready
        return 0;
    default:
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------
namespace {
const uint32_t STATE_MAGIC = 0x4A475353;   // "JGSS"
const uint32_t STATE_VERSION = 2;

struct Writer {
    std::vector<uint8_t>& out;
    void raw(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; out.insert(out.end(), b, b + n); }
    template <class T> void val(const T& v) { raw(&v, sizeof v); }
};
struct Reader {
    const std::vector<uint8_t>& in; size_t pos = 0; bool ok = true;
    void raw(void* p, size_t n) { if (pos + n > in.size()) { ok = false; return; } memcpy(p, &in[pos], n); pos += n; }
    template <class T> void val(T& v) { raw(&v, sizeof v); }
};

// Everything in Musashi's CPU struct before the first host pointer.
size_t m68kPlainBytes() { return offsetof(m68ki_cpu_core, cyc_instruction); }

template <class IO, class J> void risc(IO& io, J& r) {
    io.raw(r.regs, sizeof r.regs);
    io.val(r.pc); io.val(r.flags); io.val(r.intEnable); io.val(r.intLatch);
    io.val(r.mtxc); io.val(r.mtxa); io.val(r.endian); io.val(r.hidata); io.val(r.remain); io.val(r.divctrl);
    io.val(r.modMask); io.val(r.mac); io.val(r.running); io.val(r.singleStep); io.val(r.stepGrant);
    io.val(r.cpuIntRequest); io.val(r.branchPending); io.val(r.branchTarget); io.val(r.cycleDebt);
    io.val(r.instrCount); io.val(r.lastDest);
}

// DSA responses are short; store them in a fixed-size slot so states stay a fixed length.
template <class IO, class V> void words(IO& io, V& v) {
    uint16_t buf[64] = {0}; uint32_t n = (uint32_t)std::min<size_t>(v.size(), 64);
    for (uint32_t i = 0; i < n; i++) buf[i] = v[i];
    io.val(n); io.raw(buf, sizeof buf);
    if constexpr (!std::is_const_v<V>) { v.assign(buf, buf + std::min<uint32_t>(n, 64)); }
}

template <class IO, class B> void butch(IO& io, B& c) {
    io.val(c.ctrl); io.val(c.i2cntrl); io.val(c.sbcntrl); io.val(c.dscntrl); io.val(c.sbTime);
    io.raw(c.eeprom.mem, sizeof c.eeprom.mem);
    io.val(c.eeprom.cs); io.val(c.eeprom.clk); io.val(c.eeprom.dout); io.val(c.eeprom.writeEnable);
    io.val(c.eeprom.state); io.val(c.eeprom.bits); io.val(c.eeprom.op); io.val(c.eeprom.addr); io.val(c.eeprom.shift);
    words(io, c.resp); words(io, c.pendingResp);
    io.val(c.respPos); io.val(c.respDelay); io.val(c.respReady);
    io.val(c.gotoMin); io.val(c.gotoSec); io.val(c.speed); io.val(c.lastSession);
    io.val(c.playing); io.val(c.playFrame); io.val(c.bytePos); io.val(c.seekDelay); io.val(c.seekTarget);
    io.raw(c.fifo, sizeof c.fifo); io.val(c.fifoR); io.val(c.fifoCount); io.val(c.fifoLow);
    io.val(c.sampleAcc); io.val(c.fifoPrev); io.val(c.irqLine);
}

template <class IO, class J> void machine(IO& io, J& j) {
    io.raw(j.dram, Jaguar::DRAM_SIZE);
    io.raw(j.tom, sizeof j.tom);
    io.raw(j.jerry, sizeof j.jerry);
    risc(io, j.gpu);
    risc(io, j.dsp);
    io.val(j.tomIntEnable); io.val(j.tomIntPending); io.val(j.jerryIntEnable); io.val(j.jerryIntPending);
    io.val(j.halfLine); io.val(j.lineSlice); io.val(j.frameCount);
    io.val(j.blitBusy); io.val(j.blitIntPending);
    io.raw(j.lineBuf, sizeof j.lineBuf); io.raw(j.lineBuf32, sizeof j.lineBuf32);
    io.val(j.basePw); io.val(j.minPwSeen); io.val(j.joySelect);
    io.val(j.tomPitAcc); io.val(j.jPit1Acc); io.val(j.jPit2Acc); io.val(j.i2sAcc);
    io.raw(j.eeprom, sizeof j.eeprom);
    io.val(j.eeState); io.val(j.eeOp); io.val(j.eeBits); io.val(j.eeAddr); io.val(j.eeOutBits);
    io.val(j.eeShift); io.val(j.eeWriteEnable);
    if (j.cdUnit) {
        butch(io, j.cd);
        io.val(j.i2sRxL); io.val(j.i2sRxR); io.val(j.i2sPrevR);
    }
}
} // namespace

std::vector<uint8_t> Jaguar::saveState() const {
    std::vector<uint8_t> out;
    out.reserve(Jaguar::DRAM_SIZE + 0x20000);
    Writer w{out};
    w.val(STATE_MAGIC); w.val(STATE_VERSION); w.val(romChecksum); w.val(pal);
    uint32_t n = (uint32_t)m68kPlainBytes();
    w.val(n); w.raw(&m68ki_cpu, n);
    machine(w, *this);
    return out;
}

bool Jaguar::loadState(const std::vector<uint8_t>& st, std::string* err) {
    Reader r{st};
    uint32_t magic = 0, ver = 0, sum = 0, n = 0; bool statePal = false;
    r.val(magic); r.val(ver); r.val(sum); r.val(statePal); r.val(n);
    if (!r.ok || magic != STATE_MAGIC) { if (err) *err = "Not a JagDroid save state"; return false; }
    if (ver != STATE_VERSION) { if (err) *err = "Save state is from a different version"; return false; }
    if (sum != romChecksum) { if (err) *err = "Save state belongs to a different game"; return false; }
    if (n != m68kPlainBytes()) { if (err) *err = "Save state is from a different build"; return false; }
    // Validate the length before touching anything so a bad file can't leave a half-loaded machine.
    std::vector<uint8_t> scratch;
    {
        Writer probe{scratch}; machine(probe, *this);
        if (st.size() != r.pos + n + scratch.size()) { if (err) *err = "Save state is damaged"; return false; }
    }
    r.raw(&m68ki_cpu, n);
    machine(r, *this);
    pal = statePal;
    return r.ok;
}

} // namespace jag

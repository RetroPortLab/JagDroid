#include <cstring>
#include <vector>
extern "C" {
#include "m68k.h"
}
// JagDroid - GPU / DSP RISC interpreter.
// Instruction word: [15:10] opcode, [9:5] reg1 (source / immediate), [4:0] reg2 (destination).
#include "jaguar.h"
#include <cstdio>
#include <cstdlib>

namespace jag {

void Risc::reset() {
    memset(regs, 0, sizeof(regs));
    pc = ramBase;
    flags = 0;
    intEnable = intLatch = 0;
    mtxc = mtxa = endian = hidata = remain = divctrl = 0;
    modMask = 0xFFFFFFFF;
    mac = 0;
    running = false;
    singleStep = false; stepGrant = 0;
    cpuIntRequest = false;
    branchPending = false;
    cycleDebt = 0;
    instrCount = 0;
}

// As with TOM and JERRY, an interrupt is only latched while it is enabled.
// White Men Can't Jump forces DSP interrupt 0 while the DSP is stopped; holding
// it made the DSP run its shutdown handler as soon as it started.
void Risc::raiseInt(int n) {
    if (!(intEnable & (1u << n))) return;
    intLatch |= (1u << n);
}

// The GPU's RAM is also visible 32 KB higher ($F0B000); treat it as local.
static inline uint32_t unmirror(bool isDsp, uint32_t a) {
    if (!isDsp && (a & 0xFFC000) == 0xF08000) return a - 0x8000;
    return a;
}

uint16_t Risc::fetch16(uint32_t a) {
    a = unmirror(isDsp, a);
    uint32_t o = a - ramBase;
    if (o < ramSize) return (uint16_t)(ram[o] << 8 | ram[o + 1]);
    return jag->read16(a);
}

// Cost of a load or store that leaves the RISC's local RAM. A read waits for the
// DRAM round trip; writes are buffered. The blitter has priority on the bus, so
// accesses made while it is busy wait longer.
void Risc::extAccess(bool write, uint32_t a) {
    cycleDebt += write ? 3 : 9;
    if (jag->blitBusy > 0) cycleDebt += 6;
    // Only main-bus traffic takes time from the 68000. The GPU's reads and
    // writes of TOM's own registers (blitter, video, its control block) and
    // the DSP's of JERRY's stay inside the chip. I-War's GPU polls the blitter
    // status in a tight loop; charging those reads as bus time starved the
    // 68000 for ~50 lines, so its vertical-blank handler ran late and the
    // picture jumped for a frame (the same "shake" in Wolfenstein and Missile
    // Command 3D).
    a &= 0xFFFFFF;
    const bool onChip = isDsp ? (a >= 0xF10000 && a < 0xF20000) : (a >= 0xF00000 && a < 0xF10000);
    if (!onChip) jag->riscBusUse += write ? 2 : 4;   // bus clocks taken from the 68000
}

uint32_t Risc::load32(uint32_t a) {
    a = unmirror(isDsp, a);
    uint32_t o = (a & ~3u) - ramBase;
    if (o < ramSize) return (uint32_t)ram[o] << 24 | ram[o+1] << 16 | ram[o+2] << 8 | ram[o+3];
    extAccess(false, a);  // external bus access is slower
    // The RISCs drop the low address bits on long accesses everywhere, not
    // just in local RAM. Power Drive Rally loads a long from $580E and
    // expects the long at $580C; reading across the boundary put every
    // object ~470 lines below the screen.
    static const bool noAlL = getenv("JAG_X_NOALIGNL") != nullptr;
    if ((a & 3) && jag->verbose && getenv("JAG_UNALOG")) { static int n = 0; if (n++ < 30) fprintf(stderr, "[unaligned load] %s pc=%06X a=%06X\n", isDsp ? "DSP" : "GPU", pc, a); }
    // (TOM/JERRY register space is 16 bits wide: a long access at offset 2
    // there covers two registers, so it is not aligned.)
    return jag->read32((noAlL || (a & 0xFFFFFF) >= 0xF00000) ? a : (a & ~3u));
}

void Risc::store32(uint32_t a, uint32_t v) {
    a = unmirror(isDsp, a);
    uint32_t o = (a & ~3u) - ramBase;
    if (o < ramSize) {
        ram[o] = v >> 24; ram[o+1] = (v >> 16) & 0xFF; ram[o+2] = (v >> 8) & 0xFF; ram[o+3] = v & 0xFF;
        return;
    }
    extAccess(true, a);
    static const bool noAlS = getenv("JAG_X_NOALIGNS") != nullptr;
    if ((a & 3) && jag->verbose && getenv("JAG_UNALOG")) { static int n = 0; if (n++ < 30) fprintf(stderr, "[unaligned store] %s pc=%06X a=%06X v=%08X\n", isDsp ? "DSP" : "GPU", pc, a, v); }
    // Protector long-writes $F0002A (BORD1/BORD2); aligning it wiped VMODE.
    jag->write32((noAlS || (a & 0xFFFFFF) >= 0xF00000) ? a : (a & ~3u), v);
}

// Control registers at $F02100 (GPU) / $F1A100 (DSP)
uint32_t Risc::readCtrlReg(uint32_t off) {
    switch (off) {
    case 0x00: {  // FLAGS
        uint32_t v = flags & 0x400F;
        v |= (intEnable & 0x1F) << 4;
        if (isDsp && (intEnable & 0x20)) v |= 1u << 16;
        return v;
    }
    case 0x04: return mtxc;
    case 0x08: return mtxa;
    case 0x0C: return endian;
    case 0x10: return pc;
    case 0x14: {  // CTRL: bit0 GO, latched interrupts in bits 6..
        uint32_t v = running ? 1 : 0;
        if (singleStep) v |= 8;
        v |= (intLatch & 0x3F) << 6;
        return v;
    }
    case 0x18: return isDsp ? modMask : hidata;
    case 0x1C: return remain;
    case 0x20: return (uint32_t)((mac >> 32) & 0xFF);  // DSP MACHI
    }
    return 0;
}

void Risc::writeCtrlReg(uint32_t off, uint32_t v) {
    switch (off) {
    case 0x00: {  // FLAGS
        const uint32_t oldBank = bank();
        const bool wasMasked = (flags & IMASK) != 0;
        uint32_t keep = flags & IMASK;
        flags = (v & (ZERO | CARRY | NEG | REGPAGE));
        if ((v & IMASK) == 0) keep = 0;  // IMASK can only be cleared by software
        flags |= keep;
        intEnable = (v >> 4) & 0x1F;
        if (isDsp && (v & (1u << 16))) intEnable |= 0x20;
        intLatch &= ~((v >> 9) & 0x1F);
        if (isDsp && (v & (1u << 17))) intLatch &= ~0x20u;
        // Only a write by this processor itself is in its pipeline.
        // Only when an interrupt handler clears IMASK on its way out.
        if (wasMasked && !(flags & IMASK) && !inDelaySlot && jag->busMaster == (isDsp ? 2 : 1) && bankHold == 0 && running) { heldBank = oldBank; bankHold = 2; }
        if (heldBank == ((flags & IMASK) ? 0u : ((flags & REGPAGE) ? 32u : 0u))) bankHold = 0;
        break;
    }
    case 0x04: mtxc = v; break;
    case 0x08: mtxa = v; break;
    case 0x0C: endian = v; break;
    case 0x10: pc = v & 0xFFFFFE; branchPending = false; break;
    case 0x14: {  // CTRL
        bool wasRunning = running;
        running = (v & 1) != 0;
        // Single-step mode: the processor halts after each instruction until SINGLE_GO.
        // Games use it as a pause: the GPU sets it on itself, the 68000 does some work
        // and then writes GO+SINGLE_GO with the step bit clear to let it carry on.
        // Writing the bit from the RISC itself lets the following instruction finish.
        bool selfWrite = jag->busMaster == (isDsp ? 2 : 1);
        if (v & 8) { if (!singleStep) stepGrant = selfWrite ? 1 : 0; singleStep = true; }
        else singleStep = false;
        if (v & 16) stepGrant = 1;
        // The 68000 runs whole time slices ahead of the RISCs, but on hardware a
        // RISC it starts begins executing within a few clocks. Games rely on that:
        // they start the GPU and poll a "done" word that the GPU clears in its first
        // instructions (Trevor McFur's background decoder). Give it a short head start.
        if (running && !wasRunning) cycleDebt = 0;   // stale debt from the previous run
        if (running && !wasRunning && jag->busMaster == 0) {
            int saved = jag->busMaster;
            jag->busMaster = isDsp ? 2 : 1;
            run(32);
            jag->busMaster = saved;
        }
        if (v & 2) {  // interrupt the 68000
            if (isDsp) jag->jerryRaise(1); else jag->tomRaise(1);
        }
        if ((v & 6) && jag->verbose && getenv("JAG_CTRLLOG"))
            fprintf(stderr, "[%s] CTRL write %08X by %s at frame %llu (own pc=%06X)\n", name, v,
                    jag->busMaster == 0 ? "68k" : (jag->busMaster == 1 ? "GPU" : (jag->busMaster == 2 ? "DSP" : "other")),
                    (unsigned long long)jag->frameCount, pc);
        if (v & 4) raiseInt(0);  // force interrupt 0 (CPU interrupt)
        if (running && !wasRunning && jag->verbose && !isDsp && jag->frameCount < 40 && !getenv("JAG_TRACEDSP")) traceLeft = 30;
        if (running && !wasRunning && jag->verbose && !isDsp && getenv("JAG_TRACEGPU") && (long)jag->frameCount >= atol(getenv("JAG_TRACEGPU"))) traceLeft = 400;
        if (running && !wasRunning && jag->verbose && isDsp && getenv("JAG_TRACEDSP")) traceLeft = atoi(getenv("JAG_TRACEDSP"));
        if (!running && wasRunning && jag->verbose)
            fprintf(stderr, "[%s] stopped at $%06X by %s (frame %llu) ctrl=%08X\n", name, pc,
                    jag->busMaster == 0 ? "68k" : (jag->busMaster == 1 ? "GPU" : (jag->busMaster == 2 ? "DSP" : "other")),
                    (unsigned long long)jag->frameCount, v);
        if (running && !wasRunning && jag->verbose)
            fprintf(stderr, "[%s] start at $%06X (frame %llu VC=%d) params %08X %08X f40=%08X f44=%08X f54=%08X\n", name, pc,
                    (unsigned long long)jag->frameCount, jag->halfLine, load32(ramBase + 0x184), load32(ramBase + 0x188), load32(ramBase + 0xF40), load32(ramBase + 0xF44), load32(ramBase + 0xF54));
        break;
    }
    case 0x18: if (isDsp) modMask = v; else hidata = v; break;
    case 0x1C: divctrl = v; break;
    }
}

bool Risc::condition(int cc) const {
    bool z = flags & ZERO;
    bool cn = (cc & 0x10) ? (flags & NEG) : (flags & CARRY);
    if ((cc & 1) && z) return false;
    if ((cc & 2) && !z) return false;
    if ((cc & 4) && cn) return false;
    if ((cc & 8) && !cn) return false;
    return true;
}

void Risc::checkInterrupts() {
    if (flags & IMASK) return;
    if (bankHold > 0) return;   // not between a handler's FLAGS store and its return jump
    uint32_t pend = intLatch & intEnable;
    if (!pend) return;
    int n = 5;
    while (n >= 0 && !(pend & (1u << n))) n--;
    if (n < 0) return;
    flags |= IMASK;                        // forces bank 0
    bankHold = 0;
    uint32_t& sp = regs[31];
    sp -= 4;
    store32(sp, pc - 2);                   // handlers add 2 before returning
    // The hardware enters through an injected "movei vector,r30; jump (r30)",
    // so r30 holds the vector address; Kasumi Ninja indexes its handler table with it.
    pc = ramBase + (uint32_t)n * 16;
    if (!getenv("JAG_X_NOR30")) regs[30] = pc;
    branchPending = false;
}

uint32_t* g_pcHist = nullptr;   // diagnostics: GPU PC histogram
bool pcWatchOn = false;         // diagnostics: JAG_PCWATCH="addr,addr" + JAG_PCFRAME
void Risc::pcWatch(uint32_t addr) {
    static std::vector<uint32_t> list; static long frame = -1; static bool init = false;
    if (!init) { init = true; if (const char* w = getenv("JAG_PCWATCH")) { char b[512]; strncpy(b, w, 511); b[511] = 0;
        for (char* t = strtok(b, ","); t; t = strtok(nullptr, ",")) list.push_back(strtoul(t, nullptr, 16)); }
        if (const char* f = getenv("JAG_PCFRAME")) frame = atol(f); }
    if ((long)jag->frameCount != frame) return;
    for (uint32_t a : list) if (a == addr) {
        if (const char* d = getenv("JAG_PCDUMP")) {   // "pc,addr,len": dump memory once when that PC runs
            static bool dumped = false; unsigned dpc, da, dl;
            if (!dumped && sscanf(d, "%x,%x,%x", &dpc, &da, &dl) == 3 && dpc == addr) {
                dumped = true; fprintf(stderr, "[pcdump]");
                for (unsigned k = 0; k < dl; k += 4) fprintf(stderr, "%s%08X", k % 32 ? " " : "\n  ", jag->read32(da + k));
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "[pc %06X] r3-7=%08X %08X %08X %08X %08X ", addr, R(3), R(4), R(5), R(6), R(7));
        fprintf(stderr, "r0=%08X r1=%08X r2=%08X r14=%08X r15=%08X r16=%08X r17=%08X r18=%08X r19=%08X r22=%08X r23=%08X r24=%08X r26=%08X r27=%08X r28=%08X r30=%08X r31=%08X VC=%d\n",
            R(0), R(1), R(2), R(14), R(15), R(16), R(17), R(18), R(19), R(22), R(23), R(24), R(26), R(27), R(28), R(30), R(31), jag->halfLine);
        break;
    }
}

void Risc::run(int cycles) {
    if (!running) return;
    if (pcWatchOn && !isDsp) {
        static long sf = getenv("JAG_PCSAMPLE") ? atol(getenv("JAG_PCSAMPLE")) : -1;
        if ((long)jag->frameCount == sf && jag->lineSlice == 0) fprintf(stderr, "[sample] VC=%d pc=%06X m68k=%06X blit=%.0f\n", jag->halfLine, pc, m68k_get_reg(nullptr, M68K_REG_PC), jag->blitBusy);
    }
    if (isDsp && jag->verbose) {
        static long df = getenv("JAG_DSPSAMPLE") ? atol(getenv("JAG_DSPSAMPLE")) : -1;
        if ((long)jag->frameCount == df && jag->halfLine % 2 == 0)
            fprintf(stderr, "[dsp sample] VC=%d pc=%06X r0=%08X r1=%08X r2=%08X r3=%08X\n", jag->halfLine, pc, R(0), R(1), R(2), R(3));
    }
    int64_t budget = cycles - cycleDebt;
    cycleDebt = 0;
    while (budget > 0 && running) {
        if (singleStep) {
            if (stepGrant <= 0) { budget = 0; break; }   // halted, waiting for SINGLE_GO
            stepGrant--;
        }
        if (!branchPending) checkInterrupts();
        step();
        budget -= 1 + cycleDebt;
        cycleDebt = 0;
    }
    if (budget < 0) cycleDebt = -budget;
}

static bool oldMmAll() { static bool v = getenv("JAG_X_OLDMM") != nullptr; return v; }
static inline uint32_t quick(int n) { return n == 0 ? 32u : (uint32_t)n; }

void Risc::step() {
    if (bankHold > 0) bankHold--;   // counts down across the FLAGS store and the next instruction
    const uint32_t addr = pc;
    const uint16_t op = fetch16(addr);
    pc += 2;
    instrCount++;
    if (!isDsp && g_pcHist) g_pcHist[(addr - 0xF03000) & 0xFFF]++;
    if (jag->verbose) {   // JAG_RHIST="frame:d|g": PC histogram of the DSP or GPU over that frame
        static long hf = -2, hf2 = -1; static char who = 'd'; static long hist[2048]; static bool done = false;
        if (hf == -2) { hf = -1; if (const char* h = getenv("JAG_RHIST")) { char w = 'd'; sscanf(h, "%ld:%c:%ld", &hf, &w, &hf2); who = w; if (hf2 < hf) hf2 = hf; } }
        if (hf >= 0 && !done && (isDsp == (who == 'd'))) {
            if ((long)jag->frameCount >= hf && (long)jag->frameCount <= hf2) hist[((addr - ramBase) >> 1) & 2047]++;
            else if ((long)jag->frameCount > hf2) {
                done = true;
                for (int i = 0; i < 2048; i++) if (hist[i]) fprintf(stderr, "[rhist] %06X %ld\n", ramBase + i * 2, hist[i]);
            }
        }
    }

    const int opcode = op >> 10;
    const int r1 = (op >> 5) & 31;
    const int r2 = op & 31;
#ifdef JAG_TRACE
    { extern uint64_t g_opHist[2][64][32]; g_opHist[isDsp][opcode][r1]++; }
#endif
    if (pcWatchOn && (isDsp == (getenv("JAG_PCWATCHDSP") != nullptr))) pcWatch(addr);
    if (jag->verbose) {   // diagnostics: JAG_RISCWILD=1 reports the first jump out of local RAM
        static bool wildOn = getenv("JAG_RISCWILD") != nullptr;
        static long trapPc = getenv("JAG_RISCTRAP") ? strtol(getenv("JAG_RISCTRAP"), nullptr, 16) : -1;
        static uint32_t ring[2][64]; static int rp[2]; static bool told[2];
        if (wildOn) {
            ring[isDsp][rp[isDsp]++ & 63] = addr;
            if (!told[isDsp] && (trapPc >= 0 ? (long)addr == trapPc && (long)jag->frameCount > (getenv("JAG_TRAPFROM") ? atol(getenv("JAG_TRAPFROM")) : 0) : (addr < ramBase || addr >= ramBase + ramSize))) {
                static int left = getenv("JAG_TRAPCOUNT") ? atoi(getenv("JAG_TRAPCOUNT")) : 1;
                if (--left <= 0) told[isDsp] = true;
                fprintf(stderr, "[trap] r14=%08X (r14)=%08X r0=%08X\n", R(14), load32(R(14) & ~3u), R(0));
                fprintf(stderr, "[%s wild] pc=%06X frame %llu; previous:", name, addr, (unsigned long long)jag->frameCount);
                for (int k = 1; k <= 62; k++) { uint32_t q = ring[isDsp][(rp[isDsp] - 1 - k) & 63]; uint32_t q2 = ring[isDsp][(rp[isDsp] - k) & 63]; if (q2 - q != 2 || k < 3) fprintf(stderr, " %06X", q); }
                fprintf(stderr, "\n");
                fprintf(stderr, "   regs:"); for (int k = 0; k < 32; k++) fprintf(stderr, "%s r%d=%08X", k % 8 ? "" : "\n   ", k, R(k));
                fprintf(stderr, "\n");
            }
        }
    }
    if (traceLeft > 0) {
        traceLeft--;
        fprintf(stderr, "  %s %06X op=%2d %2d,%2d  r0=%08X r2=%08X r3=%08X r4=%08X r5=%08X Z%d C%d\n", name, addr, opcode, r1, r2,
                R(0), R(2), R(3), R(4), R(5), (flags & ZERO) ? 1 : 0, (flags & CARRY) ? 1 : 0);
    }

    // A branch scheduled by the previous instruction takes effect after this one.
    const bool takeBranch = branchPending;
    const uint32_t target = branchTarget;
    branchPending = false;
    inDelaySlot = takeBranch;

    // Pipeline interlock: an instruction that reads the register written by the
    // one before it waits a clock for the result (register scoreboard).
    {
        static const uint64_t immR1 = (1ull<<2)|(1ull<<3)|(1ull<<6)|(1ull<<7)|(1ull<<13)|(1ull<<14)|(1ull<<15)|(1ull<<24)|(1ull<<25)
            |(1ull<<27)|(1ull<<29)|(1ull<<31)|(1ull<<35)|(1ull<<38)|(1ull<<43)|(1ull<<44)|(1ull<<49)|(1ull<<50)|(1ull<<53)|(1ull<<57);
        static const uint64_t noReadR2 = (1ull<<34)|(1ull<<35)|(1ull<<36)|(1ull<<37)|(1ull<<38)|(1ull<<39)|(1ull<<40)|(1ull<<41)
            |(1ull<<42)|(1ull<<43)|(1ull<<44)|(1ull<<51)|(1ull<<52)|(1ull<<53)|(1ull<<57)|(1ull<<58)|(1ull<<59);
        static const uint64_t noWriteR2 = (1ull<<13)|(1ull<<30)|(1ull<<31)|(1ull<<45)|(1ull<<46)|(1ull<<47)|(1ull<<48)|(1ull<<49)
            |(1ull<<50)|(1ull<<52)|(1ull<<53)|(1ull<<57)|(1ull<<60)|(1ull<<61);
        const uint64_t bit = 1ull << opcode;
        if (lastDest >= 0 && ((!(immR1 & bit) && r1 == lastDest) || (!(noReadR2 & bit) && r2 == lastDest))) cycleDebt += 1;
        lastDest = (noWriteR2 & bit) ? -1 : r2;
    }

    uint32_t& D = R(r2);
    uint32_t S = R(r1);

    switch (opcode) {
    case 0: { uint32_t a = D, r = a + S; D = r; setZN(r); setC(r < a); break; }               // ADD
    case 1: { uint64_t r = (uint64_t)D + S + ((flags & CARRY) ? 1 : 0); D = (uint32_t)r; setZN(D); setC(r >> 32); break; } // ADDC
    case 2: { uint32_t a = D, r = a + quick(r1); D = r; setZN(r); setC(r < a); break; }      // ADDQ
    case 3: D += quick(r1); break;                                                            // ADDQT
    case 4: { uint32_t a = D; D = a - S; setZN(D); setC(a < S); break; }                      // SUB
    case 5: { uint64_t b = (uint64_t)S + ((flags & CARRY) ? 1 : 0); uint32_t a = D; D = (uint32_t)(a - b); setZN(D); setC((uint64_t)a < b); break; } // SUBC
    case 6: { uint32_t a = D, q = quick(r1); D = a - q; setZN(D); setC(a < q); break; }      // SUBQ
    case 7: D -= quick(r1); break;                                                            // SUBQT
    case 8: { uint32_t a = D; D = 0 - a; setZN(D); setC(a != 0); break; }                     // NEG
    case 9: D &= S; setZN(D); break;                                                          // AND
    case 10: D |= S; setZN(D); break;                                                         // OR
    case 11: D ^= S; setZN(D); break;                                                         // XOR
    case 12: D = ~D; setZN(D); break;                                                         // NOT
    case 13: flags = (flags & ~ZERO) | (((D >> r1) & 1) ? 0 : ZERO); break;                   // BTST
    case 14: D |= (1u << r1); setZN(D); break;                                                // BSET
    case 15: D &= ~(1u << r1); setZN(D); break;                                               // BCLR
    case 16: D = (D & 0xFFFF) * (S & 0xFFFF); setZN(D); break;                                // MULT
    case 17: D = (uint32_t)((int32_t)(int16_t)D * (int32_t)(int16_t)S); setZN(D); break;      // IMULT
    case 18: mac = (int64_t)(int16_t)D * (int16_t)S; setZN((uint32_t)mac); break;              // IMULTN
    case 19: D = (uint32_t)mac; break;                                                        // RESMAC
    case 20: mac += (int64_t)(int16_t)D * (int16_t)S; setZN((uint32_t)mac); break;             // IMACN
    case 21: {                                                                                // DIV
        cycleDebt += 16;
        // The divider is a 32-step non-restoring shift-and-subtract on a 33-bit
        // partial remainder, so overflowing 16.16 quotients, huge divisors and
        // division by zero come out as the hardware's bit pattern, not C's.
        const uint64_t M33 = 0x1FFFFFFFFull;
        const uint32_t dv = S;
        uint64_t wk = (divctrl & 1) ? (uint64_t)(D >> 16) : 0;
        uint32_t q = (divctrl & 1) ? (D << 16) : D;
        const uint64_t negDv = ((1ull << 33) - dv) & M33;
        uint32_t neg = 0;
        for (int i = 0; i < 32; i++) {
            wk = ((wk << 1) | (q >> 31)) & M33;
            q <<= 1;
            wk = (wk + (neg ? dv : negDv)) & M33;
            neg = (uint32_t)(wk >> 32) & 1;
            q |= neg ^ 1;
        }
        remain = (uint32_t)wk; D = q;
        if (getenv("JAG_X_OLDDIV")) {
            uint32_t dd = R(r2) ; (void)dd;
        }
        break;
    }
    case 22: { int32_t v = (int32_t)D; setC(v < 0); D = (uint32_t)(v < 0 ? -v : v); setZN(D); flags &= ~NEG; break; } // ABS
    case 23: {                                                                                // SH
        int32_t s = (int32_t)S;
        if (s >= 0) { setC(D & 1); D = s >= 32 ? 0 : D >> s; }
        else { setC(D >> 31); D = -s >= 32 ? 0 : D << -s; }
        setZN(D); break;
    }
    case 24: { int n = 32 - r1; setC(D >> 31); D = n >= 32 ? 0 : D << n; setZN(D); break; } // SHLQ
    // SHLQ stores 32-n in the instruction, but SHRQ/SHARQ store n itself (0 = 32).
    case 25: { int n = (int)quick(r1); setC(D & 1); D = n >= 32 ? 0 : D >> n; setZN(D); break; } // SHRQ
    case 26: {                                                                                // SHA
        int32_t s = (int32_t)S;
        if (s >= 0) { setC(D & 1); D = (uint32_t)((int32_t)D >> (s > 31 ? 31 : s)); }
        else { setC(D >> 31); D = -s >= 32 ? 0 : D << -s; }
        setZN(D); break;
    }
    case 27: { int n = (int)quick(r1); setC(D & 1); D = (uint32_t)((int32_t)D >> (n > 31 ? 31 : n)); setZN(D); break; } // SHARQ
    case 28: { int n = S & 31; setC(D >> 31); D = n ? (D >> n) | (D << (32 - n)) : D; setZN(D); break; } // ROR
    case 29: { int n = r1 & 31; setC(D >> 31); D = n ? (D >> n) | (D << (32 - n)) : D; setZN(D); break; } // RORQ
    case 30: { uint32_t r = D - S; setZN(r); setC(D < S); break; }                           // CMP
    case 31: { int32_t q = (r1 & 0x10) ? r1 - 32 : r1; uint32_t r = D - (uint32_t)q; setZN(r); setC(D < (uint32_t)q); break; } // CMPQ
    case 32:
        if (!isDsp) { int32_t v = (int32_t)D; D = v < 0 ? 0 : (v > 255 ? 255 : v); setZN(D); }           // SAT8
        else { uint32_t q = quick(r1), r = D - q; D = (r & ~modMask) | (D & modMask); setZN(D); }         // SUBQMOD
        break;
    case 33:
        if (!isDsp) { int32_t v = (int32_t)D; D = v < 0 ? 0 : (v > 65535 ? 65535 : v); }                  // SAT16
        else { int32_t v = (int32_t)D; D = (uint32_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }  // SAT16S
        setZN(D); break;
    case 34: D = S; break;                                                                    // MOVE
    case 35: D = (uint32_t)r1; break;                                                         // MOVEQ
    case 36: ALT(r2) = S; break;                                                              // MOVETA
    case 37: D = ALT(r1); break;                                                              // MOVEFA
    case 38: {                                                                                // MOVEI (low word first)
        uint32_t lo = fetch16(pc), hi = fetch16(pc + 2);
        pc += 4;
        cycleDebt += 2;   // the two extra instruction words cost fetch cycles
        D = (hi << 16) | lo;
        break;
    }
    // Local RAM is 32 bits wide and only takes long accesses: byte and word loads
    // and stores there act on the whole aligned long (Wolfenstein 3D relies on it).
    case 39: { uint32_t o = S - ramBase; D = (o < ramSize) ? load32(S & ~3u) : jag->read8(S); if (o >= ramSize) extAccess(false, S); break; } // LOADB
    case 40: { uint32_t o = S - ramBase; D = (o < ramSize) ? load32(S & ~3u) : jag->read16(getenv("JAG_X_NOALIGNW") ? S : (S & ~1u)); if (o >= ramSize) extAccess(false, S); break; } // LOADW
    case 41: D = load32(S); break;                                                            // LOAD
    case 42:
        if (!isDsp) { hidata = load32(S); D = load32(S + 4); }                                // LOADP
        else {                                                                                // SAT32S
            // Clamps Rn only if the 40-bit accumulator's top byte says the sum overflowed 32 bits.
            int64_t acc40 = (int64_t)((uint64_t)mac << 24) >> 24;
            int32_t top = (int32_t)(acc40 >> 32);
            if (getenv("JAG_X_OLDSAT")) { int64_t v = mac; D = (uint32_t)(v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : v)); }
            else if (top < -1) D = 0x80000000u; else if (top > 0) D = 0x7FFFFFFFu;
            setZN(D);
        }
        break;
    case 43: D = load32(R(14) + quick(r1) * 4); break;                                        // LOAD (R14+n)
    case 44: D = load32(R(15) + quick(r1) * 4); break;                                        // LOAD (R15+n)
    case 45: { uint32_t o = S - ramBase; if (o < ramSize) store32(S & ~3u, D); else { jag->write8(S, D & 0xFF); extAccess(true, S); } break; } // STOREB
    case 46: { uint32_t o = S - ramBase; if (o < ramSize) store32(S & ~3u, D); else { jag->write16(getenv("JAG_X_NOALIGNW") ? S : (S & ~1u), D & 0xFFFF); extAccess(true, S); } break; } // STOREW
    case 47: store32(S, D); break;                                                            // STORE
    case 48:
        if (!isDsp) { store32(S, hidata); store32(S + 4, D); }                                // STOREP
        else { uint32_t v = D, r = 0; for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; } D = r; setZN(D); } // MIRROR
        break;
    case 49: store32(R(14) + quick(r1) * 4, D); break;                                        // STORE (R14+n)
    case 50: store32(R(15) + quick(r1) * 4, D); break;                                        // STORE (R15+n)
    case 51: D = addr; break;                                                                 // MOVE PC
    case 52:                                                                                  // JUMP cc,(Rn)
        if (condition(r2)) { branchPending = true; branchTarget = S & 0xFFFFFE; }
        break;
    case 53: {                                                                                // JR cc,n
        int off = (r1 & 0x10) ? r1 - 32 : r1;
        if (condition(r2)) { branchPending = true; branchTarget = addr + 2 + off * 2; }
        break;
    }
    case 54: {                                                                                // MMULT
        // The vector comes from register bank 1 whatever bank is selected,
        // low half of each register first; the matrix comes from local RAM
        // through a 10-bit word counter that keeps its place for the next
        // MMULT. (Reading the other bank, high half first, saturated
        // Barkley's mixer into constant clipping.)
        int count = mtxc & 0x0F; if (count < 3) count = 3;
        uint32_t step = (mtxc & 0x10) ? (uint32_t)count * 4 : 4;
        int64_t acc = 0; uint32_t ma = oldMmAll() ? mtxa : (ramBase | (mtxa & 0xFFC));
        for (int i = 0; i < count; i++) {
            static bool oldMm = getenv("JAG_X_OLDMM") != nullptr;
            uint32_t rv = oldMm ? ALT((r1 + i / 2) & 31) : regs[32 + ((r1 + i / 2) & 31)];
            int16_t a = (int16_t)(((i & 1) ^ (oldMm ? 1 : 0)) ? (rv >> 16) : (rv & 0xFFFF));
            int16_t b = (int16_t)(load32(ma) & 0xFFFF);
            acc += (int32_t)a * b;
            ma = oldMmAll() ? ma + step : (ramBase | ((ma + step) & 0xFFC));
        }
        if (!oldMmAll()) mtxa = ma;
        D = (uint32_t)acc; setZN(D); cycleDebt += count;
        break;
    }
    case 55: D = (S & 0x007FFFFF) | ((uint32_t)((int32_t)S >> 8) & 0xFF800000); break;        // MTOI
    case 56: {                                                                                // NORMI
        // Shift that would bring the top set bit to bit 23 (-32 for zero).
        // Atari's library code builds blitter window widths from it, so an
        // off-by-one turned Ruiner Pinball's 320-pixel screen into 896.
        uint32_t v = S; int32_t r = -32;
        if (v) { r = -23; while (v >>= 1) r++; }
        if (getenv("JAG_X_OLDNORMI")) { v = S; r = 0; if (v) { while ((v & 0xFFC00000) == 0) { v <<= 1; r--; } while (v & 0xFF800000) { v >>= 1; r++; } } }
        D = (uint32_t)r; setZN(D); break;
    }
    case 57: break;                                                                           // NOP
    case 58: D = load32(R(14) + S); break;                                                    // LOAD (R14+Rn)
    case 59: D = load32(R(15) + S); break;                                                    // LOAD (R15+Rn)
    case 60: store32(R(14) + S, D); break;                                                    // STORE (R14+Rn)
    case 61: store32(R(15) + S, D); break;                                                    // STORE (R15+Rn)
    case 62:
        if (!isDsp) { int32_t v = (int32_t)D; D = v < 0 ? 0 : (v > 0xFFFFFF ? 0xFFFFFF : v); setZN(D); } // SAT24
        // (opcode 62 does nothing on the DSP)
        else if (getenv("JAG_X_OLD62")) { uint32_t r = D + quick(r1); D = (r & ~modMask) | (D & modMask); setZN(D); }
        break;
    case 63:
        if (!isDsp) {                                                                         // PACK / UNPACK
            if (r1 & 1) D = ((D & 0xF000) << 10) | ((D & 0x0F00) << 5) | (D & 0xFF);
            else D = ((D >> 10) & 0xF000) | ((D >> 5) & 0x0F00) | (D & 0xFF);
        } else {                                                                              // ADDQMOD on some revisions
            uint32_t r = D + quick(r1); D = (r & ~modMask) | (D & modMask); setZN(D);
        }
        break;
    }

    if (takeBranch) { pc = target; cycleDebt += 1; }   // taken branch refills the prefetch queue
}

} // namespace jag

#include <algorithm>
#include <array>
#include <string>
// Diagnostic runner: jagtrace <cart> [frames]
// Prints per-frame machine state and, if the 68000 leaves valid code memory,
// dumps the last instructions it executed (disassembled) and its registers.
#include "jaguar.h"
#include <map>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

extern "C" {
#include "m68k.h"
}

namespace jag { uint64_t g_opHist[2][64][32]; }
using jag::g_opHist;
static jag::Jaguar* J;
static const char* RISC_OPS[64] = {
 "add","addc","addq","addqt","sub","subc","subq","subqt","neg","and","or","xor","not","btst","bset","bclr",
 "mult","imult","imultn","resmac","imacn","div","abs","sh","shlq","shrq","sha","sharq","ror","rorq","cmp","cmpq",
 "sat8","sat16","move","moveq","moveta","movefa","movei","loadb","loadw","load","loadp","load(r14+n)","load(r15+n)","storeb",
 "storew","store","storep","store(r14+n)","store(r15+n)","move_pc","jump","jr","mmult","mtoi","normi","nop","load(r14+rn)","load(r15+rn)","store(r14+rn)","store(r15+rn)",
 "sat24","pack"};
static void dumpRisc(jag::Risc& r, unsigned from, unsigned to) {
    fprintf(stderr, "=== %s code $%06X-$%06X (pc=%06X flags=%08X) ===\n", r.name, from, to, r.pc, r.flags);
    for (unsigned a = from; a < to; ) {
        unsigned op = J->read16(a);
        int o = op >> 10, r1 = (op >> 5) & 31, r2 = op & 31;
        if (o == 38) { unsigned v = J->read16(a + 2) | (J->read16(a + 4) << 16); fprintf(stderr, "%06X  movei #$%08X, r%d\n", a, v, r2); a += 6; continue; }
        fprintf(stderr, "%06X  %-14s %2d, %2d\n", a, RISC_OPS[o], r1, r2);
        a += 2;
    }
    fprintf(stderr, "regs:");
    for (int i = 0; i < 32; i++) fprintf(stderr, "%s r%d=%08X", (i % 8 == 0) ? "\n " : "", i, r.regs[(r.flags & 0x4008) == 0x4000 ? 32 + i : i]);
    fprintf(stderr, "\n");
}

static unsigned ring[512];
static unsigned ringPos = 0;
static bool crashed = false;

static void dump() {
    fprintf(stderr, "\n=== last 68000 instructions ===\n");
    char buf[128];
    for (int i = 0; i < 512; i++) {
        unsigned pc = ring[(ringPos + i) & 511];
        if (!pc) continue;
        m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        fprintf(stderr, "%06X  %s\n", pc, buf);
    }
    fprintf(stderr, "D0-7:");
    for (int r = M68K_REG_D0; r <= M68K_REG_D7; r++) fprintf(stderr, " %08X", m68k_get_reg(nullptr, (m68k_register_t)r));
    fprintf(stderr, "\nA0-7:");
    for (int r = M68K_REG_A0; r <= M68K_REG_A7; r++) fprintf(stderr, " %08X", m68k_get_reg(nullptr, (m68k_register_t)r));
    fprintf(stderr, "\nSR=%04X\n", m68k_get_reg(nullptr, M68K_REG_SR));
    unsigned sp = m68k_get_reg(nullptr, M68K_REG_A7);
    fprintf(stderr, "vec64($100)=%08X vec26($68)=%08X  TOM int pend=%02X en=%02X  JERRY pend=%02X en=%02X\n",
            J->read32(0x100), J->read32(0x68), J->tomIntPending, J->tomIntEnable,
            J->jerryIntPending, J->jerryIntEnable);
    fprintf(stderr, "PPC=%06X\nvectors:", m68k_get_reg(nullptr, M68K_REG_PPC));
    for (int v = 0; v < 80; v++) { unsigned a = J->read32(v * 4); if (a != 0xE00020) fprintf(stderr, " [%d]=%08X", v, a); }
    fprintf(stderr, "\n");
    if (const char* w = getenv("JAG_WATCH")) {   // WATCHVAL
        unsigned wa = strtoul(w, nullptr, 16);
        fprintf(stderr, "mem[$%06X] = %08X  bytes: %02X %02X %02X %02X\n", wa, J->read32(wa),
                J->dram[wa], J->dram[wa + 1], J->dram[wa + 2], J->dram[wa + 3]);
    }
    fprintf(stderr, "stack:");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %04X", J->read16(sp + i * 2));
    fprintf(stderr, "\n");
}

static std::vector<uint8_t> g_expect;
static unsigned g_expectBase = 0x10B2A8;
static bool g_mismatch = false;
static long g_gpuFrame = -1;
static std::vector<unsigned> g_gpuWrites;
extern "C" void jag_trace_write(unsigned addr, unsigned val, int size) {
    if (g_gpuFrame >= 0 && J && (long)J->frameCount == g_gpuFrame && J->busMaster == 1)
        g_gpuWrites.push_back((addr & 0xFFFFFF) | ((unsigned)size << 28));
    static long watchAddr = -2;
    if (watchAddr == -2) { const char* w = getenv("JAG_WATCH"); watchAddr = w ? strtol(w, nullptr, 16) : -1; }
    if (watchAddr >= 0 && J) {
        unsigned a0 = addr & 0xFFFFFF;
        unsigned eff = a0 < 0x800000 ? (a0 & 0x1FFFFF) : a0;   // DRAM repeats every 2 MB
        static unsigned watchLen = 0;
        if (!watchLen) { const char* l = getenv("JAG_WATCHLEN"); watchLen = l ? strtoul(l, nullptr, 16) : 4; }
        if (eff + size > (unsigned)watchAddr && eff < (unsigned)watchAddr + watchLen) {
            static const char* whoN[] = {"68k", "GPU", "DSP", "BLITTER", "OP"};
            unsigned pc = J->busMaster == 0 ? m68k_get_reg(nullptr, M68K_REG_PPC) : (J->busMaster == 1 ? J->gpu.pc : J->dsp.pc);
            fprintf(stderr, "[watch $%06lX] %s pc=%06X wrote %0*X (size %d) at $%06X frame %llu VC=%d\n", watchAddr,
                    whoN[J->busMaster], pc, size * 2, val, size, a0, (unsigned long long)J->frameCount, J->halfLine);
            static bool traceArmed = false;
            if (getenv("JAG_WATCHTRACE") && !traceArmed && (long)J->frameCount >= (getenv("JAG_WATCHFROM") ? atol(getenv("JAG_WATCHFROM")) : 0)) {
                traceArmed = true; J->gpu.traceLeft = atoi(getenv("JAG_WATCHTRACE"));
            }
            static bool disDone = false;
            if (J->busMaster == 1 && getenv("JAG_WATCHDIS") && !disDone && (long)J->frameCount >= atol(getenv("JAG_WATCHDIS"))) {
                disDone = true; dumpRisc(J->gpu, 0xF03000, 0xF04000);
            }
            static int regDumps = 0;
            if (J->busMaster == 1 && getenv("JAG_WATCHREGS") && (long)J->frameCount >= (getenv("JAG_WATCHFROM") ? atol(getenv("JAG_WATCHFROM")) : 0) && regDumps++ < atoi(getenv("JAG_WATCHREGS"))) {
                int bank = (J->gpu.flags & jag::Risc::REGPAGE) ? 32 : 0;
                fprintf(stderr, "   flags=%08X imask=%d latch=%X bank=%d regs:", J->gpu.flags, (J->gpu.flags & 8) ? 1 : 0, J->gpu.intLatch, bank / 32);
                for (int r = 0; r < 32; r++) fprintf(stderr, "%s r%d=%08X", r % 8 ? "" : "\n   ", r, J->gpu.regs[bank + r]);
                fprintf(stderr, "\n");
            }
        }
    }
    if (J && J->busMaster == 1 && size == 1 && !g_mismatch && !g_expect.empty()) {
        unsigned o = (addr & 0xFFFFFF) - g_expectBase;
        if (o < g_expect.size() && g_expect[o] != (val & 0xFF)) {
            g_mismatch = true;
            fprintf(stderr, "[mismatch] out[%u] = %02X, expected %02X\n", o, val & 0xFF, g_expect[o]);
            dumpRisc(J->gpu, J->gpu.pc - 8, J->gpu.pc + 4);
        }
    }
    static int reported = 0;
    addr &= 0x1FFFFF;
    if (J && J->busMaster != 0 && addr >= 0x1FFF00 && addr < 0x1FFFE0 && reported < 20) {
        static const char* who[] = {"68k", "GPU", "DSP", "BLITTER", "OP"};
        if (reported == 0) dumpRisc(J->gpu, 0xF03090, 0xF03184);
        reported++;
        fprintf(stderr, "[watch] %s wrote %08X (size %d) to $%06X  frame %llu VC=%d GPU pc=%06X DSP pc=%06X\n",
                who[J->busMaster], val, size, addr, (unsigned long long)J->frameCount, J->halfLine, J->gpu.pc, J->dsp.pc);
    }
}

// Log GPU writes of the blitter pattern colour during one gameplay frame
static int g_patdFrame = -1, g_patdLogged = 0;
extern "C" void jag_trace_tomwrite(unsigned off, unsigned value, int size) {
    if (J && (off == 0x20 || off == 0x22 || off == 0x58 || (off >= 0x2270 && off < 0x228C)) && getenv("JAG_OLPLOG") && (long)J->frameCount >= atol(getenv("JAG_OLPLOG"))) {
        static const char* whoN[] = {"68k", "GPU", "DSP", "BLITTER", "OP"};
        unsigned pc = J->busMaster == 0 ? m68k_get_reg(nullptr, M68K_REG_PPC) : (J->busMaster == 1 ? J->gpu.pc : J->dsp.pc);
        fprintf(stderr, "[olp] %s pc=%06X wrote %0*X to $F000%02X frame %llu VC=%d\n", whoN[J->busMaster], pc, size * 2, value, off,
                (unsigned long long)J->frameCount, J->halfLine);
    }
    if (!J || J->busMaster != 1 || (int)J->frameCount != g_patdFrame) return;
    if ((off == 0x2268 || off == 0x226C) && g_patdLogged < 12) {
        g_patdLogged++;
        fprintf(stderr, "[patd] GPU pc=%06X wrote %08X to $F0%04X\n", J->gpu.pc, value, off);
        if (g_patdLogged == 1) dumpRisc(J->gpu, J->gpu.pc - 0x60, J->gpu.pc + 2);
    }
}

static bool g_zeroReported = false;
extern "C" void jag_trace_pc(unsigned int pc) {
    pc &= 0xFFFFFF;
    if (getenv("JAG_FIRSTPC")) {   // print each code address the first time it runs (frame, PC, disassembly)
        static std::vector<uint8_t> seen(0x1000000 / 2);
        static long limit = atol(getenv("JAG_FIRSTPC"));
        if (!seen[pc >> 1] && limit > 0) {
            seen[pc >> 1] = 1; limit--;
            char buf[128]; m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
            fprintf(stderr, "[pc] f%llu %06X  %s\n", J ? (unsigned long long)J->frameCount : 0ull, pc, buf);
        }
    }
    if (const char* bp = getenv("JAG_BREAK")) {   // "pc[,pc...]": print registers each time (up to 40)
        static std::vector<unsigned> bps; static bool init = false; static int hits = 0;
        if (!init) { init = true; char b[256]; strncpy(b, bp, 255); b[255] = 0;
            for (char* t = strtok(b, ","); t; t = strtok(nullptr, ",")) bps.push_back(strtoul(t, nullptr, 16)); }
        static long bfrom = getenv("JAG_BREAKFROM") ? atol(getenv("JAG_BREAKFROM")) : 0;
        for (unsigned a : bps) if (a == pc && hits < 40 && J && (long)J->frameCount >= bfrom) {
            hits++;
            fprintf(stderr, "[break %06X f%llu] D:", pc, J ? (unsigned long long)J->frameCount : 0ull);
            for (int r = M68K_REG_D0; r <= M68K_REG_D7; r++) fprintf(stderr, " %08X", m68k_get_reg(nullptr, (m68k_register_t)r));
            fprintf(stderr, " A:");
            for (int r = M68K_REG_A0; r <= M68K_REG_A7; r++) fprintf(stderr, " %08X", m68k_get_reg(nullptr, (m68k_register_t)r));
            unsigned sp = m68k_get_reg(nullptr, M68K_REG_A7);
            fprintf(stderr, " stk: %08X %08X %08X %08X\n", J->read32(sp), J->read32(sp + 4), J->read32(sp + 8), J->read32(sp + 12));
        }
    }
    if (const char* pl = getenv("JAG_PCLINES")) {   // "frame:vcfrom:vcto[:frame2]" 68000 PC/SR at each new half-line
        static long pf = -1, pf2 = -1; static int v0, v1, lastVc = -1;
        if (pf < 0) { if (sscanf(pl, "%ld:%d:%d:%ld", &pf, &v0, &v1, &pf2) < 4) pf2 = pf; }
        long fr = J ? (long)J->frameCount : -1;
        if (J && fr >= pf && fr <= pf2 && J->halfLine >= v0 && J->halfLine <= v1 && J->halfLine != lastVc) {
            lastVc = J->halfLine;
            fprintf(stderr, "[line f%ld vc=%d] pc=%06X sr=%04X int1en=%02X pend=%02X blit=%.0f gpu=%d@%06X\n", fr, J->halfLine, pc,
                    m68k_get_reg(nullptr, M68K_REG_SR), J->tomIntEnable, J->tomIntPending, J->blitBusy, J->gpu.running, J->gpu.pc);
        }
    }
    {   // JAG_HIST68=frame: histogram of 68000 PCs in that frame, printed at the end of it
        static long hf = getenv("JAG_HIST68") ? atol(getenv("JAG_HIST68")) : -1;
        static std::map<unsigned, long> h; static bool printed = false;
        if (J && (long)J->frameCount == hf) h[pc]++;
        else if (J && (long)J->frameCount > hf && hf >= 0 && !printed) {
            printed = true; std::vector<std::pair<long, unsigned>> v; for (auto& e : h) v.push_back({e.second, e.first});
            std::sort(v.rbegin(), v.rend()); char buf[128];
            for (size_t i = 0; i < v.size() && i < 40; i++) { m68k_disassemble(buf, v[i].second, M68K_CPU_TYPE_68000); fprintf(stderr, "[hist] %06X %6ld  %s\n", v[i].second, v[i].first, buf); }
        }
    }
    if (!g_zeroReported && getenv("JAG_ZEROEXEC") && J && J->read32(pc) == 0) {
        g_zeroReported = true;
        fprintf(stderr, "\n*** executing zeroed memory at $%06X (frame %llu)\n", pc, (unsigned long long)J->frameCount);
        char buf[128];
        for (int i = 512 - 24; i < 512; i++) {
            unsigned p = ring[(ringPos + i) & 511];
            if (!p) continue;
            m68k_disassemble(buf, p, M68K_CPU_TYPE_68000);
            fprintf(stderr, "%06X  %s\n", p, buf);
        }
        fprintf(stderr, "A0-7:");
        for (int r = M68K_REG_A0; r <= M68K_REG_A7; r++) fprintf(stderr, " %08X", m68k_get_reg(nullptr, (m68k_register_t)r));
        fprintf(stderr, "\n");
    }
    bool ok = pc < 0x200000 || (pc >= 0x800000 && pc < 0xE00000) || (pc >= 0xE00000 && pc < 0xE20000 && getenv("JAG_BIOS"));
    if (!ok && !crashed) {
        crashed = true;
        fprintf(stderr, "\n*** 68000 left code memory: PC=$%06X at frame %llu, VC=%d\n",
                pc, (unsigned long long)J->frameCount, J->halfLine);
        dump();
    }
    ring[ringPos++ & 511] = pc;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: jagtrace <cart> [frames]\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int frames = argc > 2 ? atoi(argv[2]) : 60;
    if (argc > 3) { std::ifstream e(argv[3], std::ios::binary); g_expect.assign(std::istreambuf_iterator<char>(e), std::istreambuf_iterator<char>()); }
    J = new jag::Jaguar();
    J->verbose = true;
    J->pal = getenv("JAG_PAL") != nullptr;
    jag::pcWatchOn = getenv("JAG_PCWATCH") != nullptr;
    std::string err;
    if (!J->loadCart(rom, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
    if (const char* cdp = getenv("JAG_CD")) { if (!J->insertDisc(cdp, &err)) { fprintf(stderr, "CD: %s\n", err.c_str()); return 1; } }
    if (const char* bp = getenv("JAG_BIOS")) {
        std::ifstream bf(bp, std::ios::binary); std::vector<uint8_t> b((std::istreambuf_iterator<char>(bf)), std::istreambuf_iterator<char>());
        if (!J->loadBios(b, &err)) { fprintf(stderr, "BIOS: %s\n", err.c_str()); return 1; }
    }
    J->reset();
    struct Hold { unsigned m; int a, b; };
    std::vector<Hold> holds;
    if (const char* h = getenv("JAG_HOLDS")) {   // "mask:from:to mask:from:to ..."
        unsigned m; int a, b, n;
        while (sscanf(h, "%i:%d:%d%n", &m, &a, &b, &n) == 3) { holds.push_back({m, a, b}); h += n; while (*h == ' ') h++; }
    }
    bool quiet = getenv("JAG_QUIET") != nullptr;
    if (const char* gf = getenv("JAG_GPUWRITES")) g_gpuFrame = atol(gf);
    if (const char* pf = getenv("JAG_PATD_FRAME")) g_patdFrame = atoi(pf);
    for (int fr = 0; fr < frames && !crashed; fr++) {
        unsigned m = 0;
        for (auto& hh : holds) if (fr >= hh.a && fr < hh.b) m |= hh.m;
        J->setButtons(m);
        {   // JAG_HOLDS="mask:from:to,..." presses buttons like jagrun --hold
            static std::vector<std::array<long, 3>> holds; static bool hinit = false;
            if (!hinit) { hinit = true; if (const char* h = getenv("JAG_HOLDS")) { std::string t = h; size_t p = 0;
                while (p < t.size()) { size_t q = t.find(',', p); if (q == std::string::npos) q = t.size();
                    long m, a, b; if (sscanf(t.substr(p, q - p).c_str(), "%li:%li:%li", &m, &a, &b) == 3) holds.push_back({m, a, b}); p = q + 1; } } }
            uint32_t mask = 0; for (auto& h : holds) if ((long)J->frameCount >= h[1] && (long)J->frameCount < h[2]) mask |= (uint32_t)h[0];
            if (!holds.empty()) J->setButtons(mask);
        }
        J->runFrame();
        if (const char* d = getenv("JAG_DISAT")) {   // "frame:from:to[:dsp]" disassemble GPU (or DSP) RAM after that frame
            long df; unsigned a, b; char w[8] = "";
            if (sscanf(d, "%li:%x:%x:%7s", &df, &a, &b, w) >= 3 && df == fr) dumpRisc(w[0] == 'd' ? J->dsp : J->gpu, a, b);
        }
        if (quiet) continue;
        auto r16 = [&](uint32_t a) { return J->read16(a); };
        printf("f%-3d PC=%06X SR=%04X VMODE=%04X OLP=%04X%04X VDB=%d VDE=%d VI=%d INT1en=%02X GPU=%d@%06X DSP=%d@%06X blits=%llu\n",
               fr, m68k_get_reg(nullptr, M68K_REG_PC), m68k_get_reg(nullptr, M68K_REG_SR),
               r16(0xF00028), J->tom[0x22] << 8 | J->tom[0x23], J->tom[0x20] << 8 | J->tom[0x21],
               r16(0xF00046), r16(0xF00048), r16(0xF0004E), J->tomIntEnable,
               J->gpu.running, J->gpu.pc, J->dsp.running, J->dsp.pc, (unsigned long long)J->blitCount);
    }
    if (g_gpuFrame >= 0) {
        std::map<unsigned, int> buckets;   // 64 KB buckets
        int sizes[5] = {0};
        for (unsigned w : g_gpuWrites) { buckets[(w & 0xFFFFFF) >> 16]++; sizes[w >> 28]++; }
        printf("GPU DRAM writes in frame %ld: %zu (byte %d, word %d, long %d)\n", g_gpuFrame, g_gpuWrites.size(), sizes[1], sizes[2], sizes[4]);
        for (auto& b : buckets) printf("  $%02X0000-$%02XFFFF: %d\n", b.first, b.first, b.second);
    }
    if (getenv("JAG_OPHIST")) {
        for (int o = 0; o < 64; o++) {
            uint64_t t = 0; for (int r = 0; r < 32; r++) t += g_opHist[0][o][r];
            if (t) printf("GPU op %2d %-14s %llu\n", o, RISC_OPS[o], (unsigned long long)t);
        }
        for (int o : {29, 24, 25, 27, 31, 32, 62}) {
            printf("op%d (%s) by reg1:", o, RISC_OPS[o]);
            for (int r = 0; r < 32; r++) if (g_opHist[0][o][r]) printf(" %d:%llu", r, (unsigned long long)g_opHist[0][o][r]);
            printf("\n");
        }
        printf("op63 by reg1:"); for (int r = 0; r < 32; r++) if (g_opHist[0][63][r]) printf(" r1=%d:%llu", r, (unsigned long long)g_opHist[0][63][r]);
        printf("\n");
    }
    return 0;
}

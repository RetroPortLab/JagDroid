// Jaguar CD Memory Track cartridge: the NVM BIOS module and its 128 KB of save memory.
//
// On real hardware the CD BIOS copies a 2 KB module from the cartridge into RAM at $2400
// ('_NVM' cookie at $2400, dispatcher at $2404) and CD games call it with a JSR:
//     NVM_Bios(word opcode, args...)  -> D0
// The module keeps a small filesystem in the cartridge's flash: 256 blocks of 512 bytes
// (block 0 = FAT, blocks 1-7 = directory, blocks 8+ = data).
//
// We don't have the cartridge ROM, so the module is emulated at a high level: a tiny
// 68000 stub at $2404 pokes a magic register, which runs the call here against our own
// copy of the flash, and returns. The behaviour (including its quirks) follows the
// open specification and source released as the Skunkboard Memory Track project
// (CC0-1.0, https://github.com/cubanismo/skunk_mtrk), which carries the original module.
#include "jaguar.h"
#include "m68k.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jag {

namespace {
const uint32_t NVM_BASE = 0x2400;          // module load address
const uint32_t NVM_STATE = 0x2A00;         // module variables, inside the 2 KB module space
const int BLOCKSIZE = 512, TOTALBLOCKS = 256, FIRSTDATABLOCK = 8, DATABLOCKS = 248;
const int DIROFFSET = 512, DIRENTRYSIZE = 18, NUMDIRENTRIES = 199;
const int APPPACK = 5, FILEPACK = 3, NUMFILES = 3;
const int FAT_EMPTYFILE = 2, FAT_END = 1;
const int32_t NVE_NOINIT = -1, NVE_NOSPC = -2, NVE_FILNF = -3, NVE_INVFN = -4, NVE_RANGE = -5, NVE_NFILES = -6, NVE_IHNDL = -7;

// Module variables live in guest RAM (so save states carry them):
//   +0 scratch buffer pointer (0 = Initialize not called)   +4 application name (5 packed words)
//   +14 three file handles: start block (word), offset (long)  +32 search position   +34 search flags
const uint32_t V_SCRATCH = 0, V_APP = 4, V_HANDLES = 14, V_SEARCHPOS = 32, V_SEARCHFLAGS = 34;

const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:'. ";   // 40 characters
}

struct NvmModule {
    Jaguar& j;
    explicit NvmModule(Jaguar& jag) : j(jag) {}

    // --- flash ---------------------------------------------------------------------------
    uint8_t get(uint32_t a) const { return a < j.nvm.size() ? j.nvm[a] : 0xFF; }
    void put(uint32_t a, uint8_t v) { if (a < j.nvm.size() && j.nvm[a] != v) { j.nvm[a] = v; j.nvmDirty = true; } }

    // --- guest memory --------------------------------------------------------------------
    uint32_t r32(uint32_t a) { return j.read32(a); }
    uint16_t r16(uint32_t a) { return j.read16(a); }
    uint8_t r8(uint32_t a) { return j.read8(a); }
    void w32(uint32_t a, uint32_t v) { j.write32(a, v); }
    void w16(uint32_t a, uint16_t v) { j.write16(a, v); }
    void w8(uint32_t a, uint8_t v) { j.write8(a, v); }

    // --- names ---------------------------------------------------------------------------
    static int five(int c, bool& eos) {
        if (c == 0) eos = true;
        if (eos) return 39;
        if (c <= 'z' && c - 'a' >= 0) return c - 'a';
        for (int i = 0; i < 39; i++) if ((uint8_t)CHARSET[i] == c) return i;
        return 39;
    }
    // Pack a guest string into 'words' 16-bit words of three characters each.
    void pack(uint32_t str, int words, uint16_t* out) {
        bool eos = false;
        uint32_t p = str;
        for (int w = 0; w < words; w++) {
            int v = 0;
            for (int k = 0; k < 3; k++) {
                int c = eos ? 0 : r8(p++);
                v = k == 0 ? five(c, eos) : v * 40 + five(c, eos);
            }
            out[w] = (uint16_t)v;
        }
    }
    void unpack(const uint16_t* in, int words, uint32_t dst) {
        for (int w = 0; w < words; w++) {
            unsigned v = in[w];
            w8(dst + w * 3 + 2, (uint8_t)CHARSET[v % 40]);
            v /= 40;
            w8(dst + w * 3 + 1, (uint8_t)CHARSET[v % 40]);
            v /= 40;
            w8(dst + w * 3, (uint8_t)CHARSET[v % 40]);
        }
        w8(dst + words * 3, 0);
    }

    // --- filesystem ----------------------------------------------------------------------
    struct Dir { uint8_t start, blocks; uint16_t app[APPPACK], file[FILEPACK]; };

    uint16_t checksum() const {
        uint16_t s = 0;
        for (int i = 2; i < BLOCKSIZE; i++) s = (uint16_t)(s + get(i));
        return s;
    }
    uint16_t setChecksum() { uint16_t s = checksum(); put(0, s >> 8); put(1, s & 0xFF); return s; }

    Dir fetchDir(int n) const {
        uint32_t a = DIROFFSET + n * DIRENTRYSIZE;
        Dir d; d.start = get(a); d.blocks = get(a + 1);
        for (int i = 0; i < APPPACK; i++) d.app[i] = (uint16_t)(get(a + 2 + i * 2) << 8 | get(a + 3 + i * 2));
        for (int i = 0; i < FILEPACK; i++) d.file[i] = (uint16_t)(get(a + 12 + i * 2) << 8 | get(a + 13 + i * 2));
        return d;
    }
    void putDir(int n, const Dir& d) {
        uint32_t a = DIROFFSET + n * DIRENTRYSIZE;
        put(a, d.start); put(a + 1, d.blocks);
        for (int i = 0; i < APPPACK; i++) { put(a + 2 + i * 2, d.app[i] >> 8); put(a + 3 + i * 2, d.app[i] & 0xFF); }
        for (int i = 0; i < FILEPACK; i++) { put(a + 12 + i * 2, d.file[i] >> 8); put(a + 13 + i * 2, d.file[i] & 0xFF); }
    }

    // First block of the allocation, or a negative error. Blocks are taken from anywhere in the FAT.
    int allocBlocks(int n) {
        if (n == 0) return FAT_EMPTYFILE;
        int used = get(3);
        if (used + n > DATABLOCKS) return NVE_NOSPC;
        put(3, (uint8_t)(used + n));
        int first = 0, prev = 0, left = n;
        for (int b = FIRSTDATABLOCK; b < TOTALBLOCKS && left; b++) {
            if (get(b) != 0) continue;
            if (prev) put(prev, (uint8_t)b); else first = b;
            prev = b; left--;
        }
        if (left) return NVE_RANGE;
        put(prev, FAT_END);
        return first;
    }
    void freeBlocks(int start) {
        int d2 = start, used = get(3);
        while (used != 0) {
            if (d2 <= FAT_EMPTYFILE) break;
            int next = get(d2);
            put(d2, 0);
            d2 = next;
            used--;
        }
        put(3, (uint8_t)used);
    }
    int findFile(const uint16_t* app, const uint16_t* file, Dir& out) const {
        for (int n = 0; n < NUMDIRENTRIES; n++) {
            Dir d = fetchDir(n);
            if (d.start == 0 && d.blocks == 0) continue;
            if (memcmp(d.app, app, sizeof d.app) != 0 || memcmp(d.file, file, sizeof d.file) != 0) continue;
            out = d;
            return n;
        }
        return NVE_FILNF;
    }
    int deleteFile(const uint16_t* app, const uint16_t* file) {
        Dir d;
        int n = findFile(app, file, d);
        if (n < 0) return n;
        uint32_t a = DIROFFSET + n * DIRENTRYSIZE;
        put(a, 0); put(a + 1, 0);
        freeBlocks(d.start);
        return 0;
    }
    // Byte offset in a file -> absolute address in the flash, or NVE_RANGE.
    int64_t fileOffset(int startBlock, int32_t off) const {
        if (off < 0) return NVE_RANGE;
        int blk = startBlock;
        while (off >= BLOCKSIZE) {
            if (blk < FAT_EMPTYFILE) return NVE_RANGE;
            blk = get(blk);
            off -= BLOCKSIZE;
        }
        if (blk < FAT_EMPTYFILE) return NVE_RANGE;
        return (int64_t)blk * BLOCKSIZE + off;
    }

    // --- per-call state ------------------------------------------------------------------
    uint32_t hAddr(int h) { return NVM_STATE + V_HANDLES + h * 6; }
    int getHandle() {
        for (int h = 0; h < NUMFILES; h++) if (r16(hAddr(h)) == 0) return h;
        return NVE_NFILES;
    }
    void setHandle(int h, int start) { w16(hAddr(h), (uint16_t)start); w32(hAddr(h) + 2, 0); }
    void appName(uint16_t* out) { for (int i = 0; i < APPPACK; i++) out[i] = r16(NVM_STATE + V_APP + i * 2); }

    // --- the dispatcher ------------------------------------------------------------------
    int32_t call(uint32_t sp) {
        int op = (int16_t)r16(sp + 4);
        const uint32_t S = NVM_STATE;
        if (op == 0) return init(sp);
        if (r32(S + V_SCRATCH) == 0) return NVE_NOINIT;
        switch (op) {
        case 1: return create(sp);
        case 2: return open(sp);
        case 3: {
            unsigned h = r16(sp + 6);
            if (h < (unsigned)NUMFILES) { w16(hAddr(h), 0); w32(hAddr(h) + 2, 0); }
            return 0;
        }
        case 4: return del(sp);
        case 5: case 6: return rw(sp, op == 6);
        case 7: return sfirst(sp);
        case 8: return snext(r32(sp + 6));
        case 9: return seek(sp);
        case 10: {
            w32(r32(sp + 6), DATABLOCKS * BLOCKSIZE);
            w32(r32(sp + 10), (uint32_t)((DATABLOCKS - get(3)) & 0xFF) * BLOCKSIZE);
            return 0;
        }
        default: return NVE_INVFN;
        }
    }

    int32_t init(uint32_t sp) {
        const uint32_t S = NVM_STATE;
        uint16_t app[APPPACK];
        pack(r32(sp + 6), APPPACK, app);
        for (int i = 0; i < APPPACK; i++) w16(S + V_APP + i * 2, app[i]);
        w32(S + V_SCRATCH, r32(sp + 10));
        for (int h = 0; h < NUMFILES; h++) { w16(hAddr(h), 0); w32(hAddr(h) + 2, 0); }
        w16(S + V_SEARCHPOS, 0); w16(S + V_SEARCHFLAGS, 0);
        uint16_t stored = (uint16_t)(get(0) << 8 | get(1));
        if (stored != checksum()) {                 // blank or damaged: format the FAT and directory
            for (int a = 0; a < (1 + 7) * BLOCKSIZE; a++) put(a, 0);
        }
        return 0;
    }

    int32_t create(uint32_t sp) {
        int h = getHandle();
        if (h < 0) return h;
        uint16_t app[APPPACK], file[FILEPACK];
        appName(app);
        pack(r32(sp + 6), FILEPACK, file);
        deleteFile(app, file);
        int n = 0;
        Dir d;
        for (; n < NUMDIRENTRIES; n++) { d = fetchDir(n); if (d.start == 0) break; }
        if (n == NUMDIRENTRIES) return NVE_NOSPC;
        uint32_t size = r32(sp + 10);
        uint32_t blocks = (size + BLOCKSIZE - 1) >> 9;
        int start = allocBlocks((int)blocks);
        if (start < 0) return NVE_NOSPC;
        d.start = (uint8_t)start; d.blocks = (uint8_t)blocks;
        memcpy(d.app, app, sizeof app); memcpy(d.file, file, sizeof file);
        putDir(n, d);
        setChecksum();
        setHandle(h, start);
        return h;
    }
    int32_t open(uint32_t sp) {
        int h = getHandle();
        if (h < 0) return h;
        uint16_t app[APPPACK], file[FILEPACK];
        appName(app);
        pack(r32(sp + 6), FILEPACK, file);
        Dir d;
        int n = findFile(app, file, d);
        if (n < 0) return n;
        setHandle(h, d.start);
        return h;
    }
    int32_t del(uint32_t sp) {
        uint16_t app[APPPACK], file[FILEPACK];
        uint32_t appPtr = r32(sp + 6);
        if (appPtr) pack(appPtr, APPPACK, app); else appName(app);
        pack(r32(sp + 10), FILEPACK, file);
        deleteFile(app, file);
        return setChecksum();   // the module returns the new checksum here, always non-negative
    }
    int32_t seek(uint32_t sp) {
        unsigned h = r16(sp + 6);
        if (h >= (unsigned)NUMFILES) return NVE_IHNDL;
        uint16_t start = r16(hAddr(h));
        if (start == 0) return NVE_IHNDL;
        int32_t off = (int32_t)r32(sp + 8);
        int flag = (int16_t)r16(sp + 12);
        if (flag == 1) off += (int32_t)r32(hAddr(h) + 2);
        else if (flag != 0) return NVE_INVFN;
        int64_t a = fileOffset(start, off);
        if (a < 0) return (int32_t)a;
        w32(hAddr(h) + 2, (uint32_t)off);
        return off;
    }
    int32_t rw(uint32_t sp, bool write) {
        unsigned h = r16(sp + 6);
        if (h >= (unsigned)NUMFILES) return NVE_IHNDL;
        uint16_t start = r16(hAddr(h));
        if (start == 0) return NVE_IHNDL;
        int32_t off = (int32_t)r32(hAddr(h) + 2);
        uint32_t buf = r32(sp + 8);
        int32_t limit = (int32_t)r32(sp + 12), done = 0;
        int32_t left = 0;     // bytes left in the current block
        uint32_t addr = 0;
        while (done < limit) {
            addr++;
            if (--left < 0) {
                int64_t a = fileOffset(start, off);
                if (a < 0) break;
                addr = (uint32_t)a;
                left = (~addr) & (BLOCKSIZE - 1);
            }
            off++; done++;
            if (write) put(addr, r8(buf++)); else w8(buf++, get(addr));
        }
        w32(hAddr(h) + 2, (uint32_t)off);
        return done;
    }
    int32_t sfirst(uint32_t sp) {
        uint32_t flags = r32(sp + 10);
        if (flags != 0 && (flags & 0xFFFF) != 1) return NVE_INVFN;
        w16(NVM_STATE + V_SEARCHFLAGS, (uint16_t)(flags & 0xFFFF));
        w16(NVM_STATE + V_SEARCHPOS, 0);
        return snext(r32(sp + 6));
    }
    int32_t snext(uint32_t buf) {
        const uint32_t S = NVM_STATE;
        uint16_t mine[APPPACK];
        appName(mine);
        for (;;) {
            int n = r16(S + V_SEARCHPOS);
            if (n >= NUMDIRENTRIES) return NVE_FILNF;
            w16(S + V_SEARCHPOS, (uint16_t)(n + 1));
            Dir d = fetchDir(n);
            if (d.start == 0) continue;
            if (r16(S + V_SEARCHFLAGS) != 0 && memcmp(d.app, mine, sizeof mine) != 0) continue;
            w32(buf, (uint32_t)d.blocks << 9);
            unpack(d.app, APPPACK, buf + 4);
            unpack(d.file, FILEPACK, buf + 4 + 16);
            return 0;
        }
    }
};

// The 68000 stub at $2404: poke the magic register ($DFFFE0), then return.
static const uint8_t NVM_STUB[] = {
    0x5F, 0x4E, 0x56, 0x4D,                  // '_NVM'
    0x33, 0xFC, 0x00, 0x00, 0x00, 0xDF, 0xFF, 0xE0,      // move.w #0,$DFFFE0.l
    0x4E, 0x75,                                          // rts
};

void Jaguar::setNvm(const uint8_t* data, size_t len) {
    nvm.assign(NVM_SIZE, 0xFF);   // a blank cartridge reads as all ones and formats itself on first use
    if (data) memcpy(nvm.data(), data, std::min<size_t>(len, NVM_SIZE));
    nvmDirty = false;
}

void Jaguar::nvmInstall() {
    if (!cdUnit || !nvmEnabled) return;
    if (nvm.empty()) setNvm(nullptr, 0);
    for (uint32_t a = 0; a < 0x800; a++) dram[NVM_BASE + a] = 0;
    memcpy(&dram[NVM_BASE], NVM_STUB, sizeof NVM_STUB);
    nvmInstalled = true;
}

void Jaguar::nvmCall() {
    uint32_t sp = m68k_get_reg(nullptr, M68K_REG_SP);
    NvmModule m(*this);
    int32_t r = m.call(sp);
    static const bool log = getenv("JAG_NVMLOG") != nullptr;
    if (log) fprintf(stderr, "[nvm] f%ld op=%d -> %d\n", (long)frameCount, (int)(int16_t)read16(sp + 4), (int)r);
    m68k_set_reg(M68K_REG_D0, (uint32_t)r);
}

} // namespace jag

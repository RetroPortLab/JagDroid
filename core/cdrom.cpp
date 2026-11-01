// Jaguar CD support: .cue/.bin disc images and the Butch controller.
//
// Register map at $DFFF00 (from Atari's Jaguar CD documentation):
//   +$00 BUTCH     interrupt control (write) / status (read: bit 9 FIFO half full,
//                  bit 12 command buffer empty, bit 13 response waiting)
//   +$04 DSCNTRL   DSA control
//   +$0A DS_DATA   DSA command (write) / response (read)
//   +$10 I2CNTRL   I2S control (bit 2 enables the data FIFO); read: bit 0 drive
//                  streaming, bit 1 path to JERRY, bit 3 16-bit bus, bit 4 FIFO not empty
//   +$14 SBCNTRL, +$18 SUBDATA, +$1C SUBDATB, +$20 SB_TIME   subcode
//   +$24 FIFO_DATA, +$28 I2SDAT2   disc data
//   +$2C           CD unit EEPROM
//
// DSA responses follow the drive's command set: $01xx found, $02xx stopped,
// $03xx disc status, $04xx error, $17xx mode, $20-$24 session TOC, $54xx
// session count, $60-$65 long TOC per track.
#include "cdrom.h"
#include "jaguar.h"
#include "zipfile.h"
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace jag {

// ---------------------------------------------------------------------------
// Disc image
// ---------------------------------------------------------------------------
static int msf(int m, int s, int f) { return (m * 60 + s) * 75 + f; }

CdImage::~CdImage() { for (auto& t : tracks) if (t.fp) fclose(t.fp); }

bool CdImage::load(const std::string& cuePath, std::string* err) {
    std::ifstream in(cuePath);
    if (!in) { if (err) *err = "Can't open " + cuePath; return false; }
    std::stringstream ss; ss << in.rdbuf();
    std::string dir = cuePath.substr(0, cuePath.find_last_of("/\\") + 1);
    return loadText(ss.str(), [dir](const std::string& name) {
        Opened o; o.fp = fopen((dir + name).c_str(), "rb"); return o;
    }, err);
}

bool CdImage::loadText(const std::string& cueText, const Opener& open, std::string* err) {
    std::istringstream in(cueText);
    struct Pending { std::string file; int number = 0; bool data = false; int index0 = -1, index1 = -1; int session = 1; };
    std::vector<Pending> list;
    std::string line, curFile;
    int session = 1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream ls(line);
        std::string kw; ls >> kw;
        if (kw == "REM") { std::string what; int n; ls >> what >> n; if (what == "SESSION") session = n; }
        else if (kw == "FILE") {
            size_t a = line.find('"'), b = line.rfind('"');
            curFile = (a != std::string::npos && b > a) ? line.substr(a + 1, b - a - 1) : "";
        } else if (kw == "TRACK") {
            Pending p; std::string type; ls >> p.number >> type;
            p.file = curFile; p.data = type.rfind("MODE", 0) == 0; p.session = session;
            list.push_back(p);
        } else if (kw == "INDEX" && !list.empty()) {
            int idx; char c1, c2; int m, s, f; ls >> idx >> m >> c1 >> s >> c2 >> f;
            (idx == 0 ? list.back().index0 : list.back().index1) = msf(m, s, f);
        }
    }
    if (list.empty()) { if (err) *err = "No tracks in the cue sheet"; return false; }

    // Lay the tracks out on the disc. Each file holds one track; within a session
    // they follow each other directly. A new session starts after the previous
    // one's lead-out (1:30) and its own lead-in (1:00) plus the 2-second pregap.
    int pos = 150, curSession = list[0].session;
    for (size_t i = 0; i < list.size(); i++) {
        Track t;
        t.number = list[i].number; t.session = list[i].session; t.data = list[i].data;
        t.path = list[i].file;
        bool shared = !tracks.empty() && tracks.back().path == t.path;
        if (shared) {
            if (err) *err = "Cue sheets with several tracks in one file aren't supported yet";
            return false;
        }
        Opened of = open(t.path);
        t.fp = of.fp; t.base = of.base;
        if (!t.fp) { if (err) *err = "Missing track file: " + list[i].file; return false; }
        long size = of.size;
        if (size < 0) { fseek(t.fp, 0, SEEK_END); size = ftell(t.fp) - of.base; }
        t.frames = (int)(size / 2352);
        if (sessions.empty()) { Session s; s.firstTrack = t.number; sessions.push_back(s); }
        if (t.session != curSession) {
            sessions.back().leadout = pos;
            pos += 6750 + 4500 + 150;
            curSession = t.session;
            Session s; s.firstTrack = t.number; sessions.push_back(s);
        }
        sessions.back().lastTrack = t.number;
        int idx1 = list[i].index1 < 0 ? 0 : list[i].index1;
        t.fileStart = pos;
        t.start = pos + idx1;
        pos += t.frames;
        tracks.push_back(t);
    }
    sessions.back().leadout = pos;
    return true;
}

bool CdImage::loadZip(const std::function<FILE*()>& openZip, const std::string& cacheDir,
                      const std::function<bool(double)>& progress, std::string* err) {
    FILE* zf = openZip();
    if (!zf) { if (err) *err = "Can't open the zip"; return false; }
    ZipArchive zip;
    if (!zip.open(zf, err)) { fclose(zf); return false; }
    const ZipEntry* cue = nullptr;
    for (auto& e : zip.entries()) if (endsWithNoCase(e.name, ".cue")) { cue = &e; break; }
    if (!cue) { fclose(zf); if (err) *err = "No .cue sheet in the zip"; return false; }
    std::vector<uint8_t> cueBytes;
    if (!zip.readAll(*cue, cueBytes, err)) { fclose(zf); return false; }
    std::string text(cueBytes.begin(), cueBytes.end());

    // Total compressed size still to unpack, for an overall progress figure.
    uint64_t toUnpack = 0, unpacked = 0;
    auto cachedPath = [&](const ZipEntry& e) { return cacheDir + "/" + e.baseName(); };
    auto alreadyCached = [&](const ZipEntry& e) {
        struct stat st; return stat(cachedPath(e).c_str(), &st) == 0 && (uint64_t)st.st_size == e.usize;
    };
    for (auto& e : zip.entries())
        if (e.method != 0 && !endsWithNoCase(e.name, ".cue") && !alreadyCached(e)) toUnpack += e.csize;

    bool ok = loadText(text, [&](const std::string& name) {
        Opened o;
        const ZipEntry* e = zip.find(name);
        if (!e) return o;
        if (e->method == 0) {            // stored: read straight out of the zip
            uint64_t off;
            if (!zip.dataOffset(*e, off, err)) return o;
            o.fp = openZip(); o.base = (long)off; o.size = (long)e->usize;
            return o;
        }
        std::string path = cachedPath(*e);
        if (!alreadyCached(*e)) {
            if (cacheDir.empty()) { if (err) *err = "Compressed disc needs a cache folder"; return o; }
            mkdir(cacheDir.c_str(), 0755);
            uint64_t base = unpacked;
            bool done = zip.extractTo(*e, path, [&](double f) {
                return !progress || progress(toUnpack ? (base + f * e->csize) / (double)toUnpack : 1.0);
            }, err);
            unpacked += e->csize;
            if (!done) return o;
        }
        o.fp = fopen(path.c_str(), "rb");
        return o;
    }, err);
    fclose(zf);
    return ok;
}

int CdImage::trackAt(int frame) const {
    for (size_t i = 0; i < tracks.size(); i++)
        if (frame >= tracks[i].fileStart && frame < tracks[i].fileStart + tracks[i].frames) return (int)i;
    return -1;
}

void CdImage::readSector(int frame, uint8_t* out) {
    int t = trackAt(frame);
    if (t < 0) { memset(out, 0, 2352); return; }
    const Track& tr = tracks[t];
    if (!tr.fp || fseek(tr.fp, tr.base + (long)(frame - tr.fileStart) * 2352, SEEK_SET) != 0 ||
        fread(out, 1, 2352, tr.fp) != 2352)
        memset(out, 0, 2352);
}

// ---------------------------------------------------------------------------
// CD unit EEPROM
// ---------------------------------------------------------------------------
enum { SE_IDLE, SE_OP, SE_ADDR, SE_READ, SE_WRITE, SE_WRAL };

void SerialEeprom::write(uint16_t v) {
    // Chip select is active low on the CD unit: the BIOS pulses bit 0 high to
    // reset the chip and clocks a whole command with it held low.
    bool ncs = !(v & 1), nclk = (v >> 1) & 1, di = (v >> 2) & 1;
    if (!ncs) {                          // deselect: back to idle, report ready
        cs = false; clk = nclk; state = SE_IDLE; dout = true;
        return;
    }
    bool rising = ncs && cs && nclk && !clk;
    cs = ncs; clk = nclk;
    if (!rising) return;
    switch (state) {
    case SE_IDLE: if (di) { state = SE_OP; bits = 0; op = 0; } break;   // start bit
    case SE_OP: op = (op << 1) | di; if (++bits == 2) { state = SE_ADDR; bits = 0; addr = 0; } break;
    case SE_ADDR:
        addr = (addr << 1) | di;
        if (++bits == 6) {
            bits = 0; shift = 0;
            if (op == 2) { state = SE_READ; dout = false; shift = mem[addr]; bits = 16; }   // dummy 0, then data
            else if (op == 1) state = SE_WRITE;
            else if (op == 3) { if (writeEnable) mem[addr] = 0xFFFF; state = SE_IDLE; }
            else switch (addr >> 4) {
                case 3: writeEnable = true; state = SE_IDLE; break;
                case 0: writeEnable = false; state = SE_IDLE; break;
                case 2: if (writeEnable) for (auto& w : mem) w = 0xFFFF; state = SE_IDLE; break;
                default: state = SE_WRAL; break;
            }
        }
        break;
    case SE_READ:
        if (bits > 0) { bits--; dout = (shift >> bits) & 1; }
        else dout = true;
        break;
    case SE_WRITE:
    case SE_WRAL:
        shift = (shift << 1) | di;
        if (++bits == 16) {
            if (writeEnable) { if (state == SE_WRITE) mem[addr] = (uint16_t)shift; else for (auto& w : mem) w = (uint16_t)shift; }
            state = SE_IDLE; dout = true;
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// Butch
// ---------------------------------------------------------------------------
static uint64_t g_cdReads[128];
void Butch::debugDump() {
    fprintf(stderr, "[cd] overflows %llu pos %d playing %d fifo %d words %llu i2c=%X ctrl=%X resp %d/%zu\n", (unsigned long long)fifoOverflows, playFrame, playing, fifoCount,
            (unsigned long long)dataWords, i2cntrl, ctrl, (int)respPos, resp.size());
    for (int i = 0; i < 128; i++) if (g_cdReads[i]) fprintf(stderr, "   rd %02X x%llu", i * 2, (unsigned long long)g_cdReads[i]);
    fprintf(stderr, "\n");
}
void Butch::reset() {
    ctrl = 0x40000; i2cntrl = 0;   // power-up: CD BIOS mapped into cartridge space
    sbcntrl = 0; dscntrl = 0; sbTime = 0;
    eeprom.cs = eeprom.clk = false; eeprom.state = 0; eeprom.dout = true;
    resp.clear(); respPos = 0; respReady = false; respDelay = 0; pendingResp.clear();
    playing = false; playFrame = 150; bytePos = 0; sectorFrame = -1; seekDelay = 0;
    fifoClear(); sampleAcc = 0; irqLine = false; speed = 2;
}

void Butch::respond(std::vector<uint16_t> words, double delay) {
    pendingResp = std::move(words);
    respDelay = std::max(1.0, delay);
    respReady = false;
}

static const double CMD_DELAY = 2000;   // ~75 us for the drive to answer

void Butch::command(uint16_t cmd) {
    int op = cmd >> 8, arg = cmd & 0xFF;
    if (jag->verbose && getenv("JAG_CDLOG")) fprintf(stderr, "[cd] DSA command %04X frame %llu (pos %d words %llu)\n", cmd, (unsigned long long)jag->frameCount, playFrame, (unsigned long long)dataWords);
    if (jag->verbose && getenv("JAG_CDLOG")) { for (int i = 0; i < 128; i++) if (g_cdReads[i]) fprintf(stderr, "   rd %02X x%llu", i * 2, (unsigned long long)g_cdReads[i]); fprintf(stderr, " i2c=%X ctrl=%X\n", i2cntrl, ctrl); }
    if (!disc) { respond({0x0400 | 0x02}, CMD_DELAY); return; }   // focus error: no disc
    auto toMsf = [](int frame, int& m, int& s, int& f) { m = frame / 4500; s = (frame / 75) % 60; f = frame % 75; };
    switch (op) {
    case 0x01: {                                   // play title
        for (auto& t : disc->tracks) if (t.number == arg) { playFrame = t.start; bytePos = 0; playing = true; }
        respond({(uint16_t)(0x0100 | arg)}, CMD_DELAY * 20);
        break;
    }
    case 0x02: playing = false; seekDelay = 0; respond({0x0200}, CMD_DELAY); break;   // stop
    case 0x03: {                                   // session TOC
        if (arg >= (int)disc->sessions.size()) { respond({0x0400 | 0x29}, CMD_DELAY); break; }
        auto& s = disc->sessions[arg];
        int m, sec, f; toMsf(s.leadout, m, sec, f);
        respond({(uint16_t)(0x2000 | s.firstTrack), (uint16_t)(0x2100 | s.lastTrack), (uint16_t)(0x2200 | m),
                 (uint16_t)(0x2300 | sec), (uint16_t)(0x2400 | f)}, CMD_DELAY);
        break;
    }
    case 0x04: respond({0x0141}, CMD_DELAY); break;                 // pause
    case 0x05: respond({0x0142}, CMD_DELAY); break;                 // pause release
    case 0x10: gotoMin = arg; break;                               // goto time: minutes
    case 0x11: gotoSec = arg; break;                               // seconds
    case 0x12: {                                                   // frames: seek
        int target = msf(gotoMin, gotoSec, arg);
        int dist = std::abs(target - playFrame);
        seekTarget = target;
        playing = false;
        fifoClear();
        seekDelay = 26590906.0 * (0.008 + dist / 300000.0);       // 8 ms plus travel
        break;
    }
    case 0x14: {                                                   // long TOC of a session
        if (arg >= (int)disc->sessions.size()) { respond({0x0400 | 0x29}, CMD_DELAY); break; }
        std::vector<uint16_t> w;
        for (auto& t : disc->tracks) {
            if (t.session != arg + 1) continue;
            int m, sec, f; toMsf(t.start, m, sec, f);
            w.push_back((uint16_t)(0x6000 | t.number));
            w.push_back((uint16_t)(0x6100 | (t.data ? 0x41 : 0x01)));
            w.push_back((uint16_t)(0x6200 | m));
            w.push_back((uint16_t)(0x6300 | sec));
            w.push_back((uint16_t)(0x6400 | f));
            w.push_back((uint16_t)(0x6500 | arg));
        }
        respond(w, CMD_DELAY);
        break;
    }
    case 0x15: speed = (arg & 3) == 1 ? 1 : 2; respond({(uint16_t)(0x1700 | arg)}, CMD_DELAY); break;  // set mode
    case 0x16: respond({0x0400}, CMD_DELAY); break;                // last error: none
    case 0x17: respond({0x0400}, CMD_DELAY); break;                // clear error
    case 0x18: lastSession = arg; respond({0x0100}, CMD_DELAY * 50); break;   // spin up
    case 0x50: respond({0x0300}, CMD_DELAY); break;                // disc present
    case 0x51: respond({(uint16_t)(0x5100 | arg)}, CMD_DELAY); break;
    case 0x54: respond({(uint16_t)(0x5400 | disc->sessions.size())}, CMD_DELAY); break;
    case 0x6A: respond({0x6A00}, CMD_DELAY); break;
    case 0x70: respond({(uint16_t)(0x7000 | arg)}, CMD_DELAY); break;
    default: respond({0x0400 | 0x28}, CMD_DELAY); break;           // illegal command
    }
}

uint16_t Butch::nextDataWord() {
    if (!disc) return 0;
    if (sectorFrame != playFrame) { disc->readSector(playFrame, sector); sectorFrame = playFrame; }
    // CD audio samples are little-endian in the image; the Jaguar sees big-endian words.
    uint16_t w = (uint16_t)(sector[bytePos + 1] << 8 | sector[bytePos]);
    bytePos += 2;
    dataWords++;
    if (bytePos >= 2352) { bytePos = 0; playFrame++; }
    return w;
}

void Butch::produceSample() {
    uint16_t w0 = 0, w1 = 0;
    if (playing && disc) { w0 = nextDataWord(); w1 = nextDataWord(); }
    if (jag->verbose && getenv("JAG_CDFIFO") && playing && (playFrame == atoi(getenv("JAG_CDFIFO")) || playFrame == atoi(getenv("JAG_CDFIFO")) + 1) && bytePos < 400 + 4)
        fprintf(stderr, "[fifo] f%d b%d %04X%04X cnt=%d\n", playFrame, bytePos, w0, w1, fifoCount);
    if (playing && (i2cntrl & 4)) {
        if (fifoCount < 16) { fifo[(fifoR + fifoCount) & 15] = (uint32_t)fifoPrev << 16 | w0; fifoCount++; }
        else fifoOverflows++;
    }
    fifoPrev = w1;
    // Disc data reaches JERRY as the I2S frame; the BIOS boot loader reads it there.
    jag->cdI2sFrame(w0, w1);
}

uint16_t Butch::read16(uint32_t off) {
    g_cdReads[(off & 0xFE) >> 1]++;
    if (jag->verbose && getenv("JAG_CDLOG") && (off & 0xFE) != 0x24 && (off & 0xFE) != 0x26) {
        static int n = 0; if (n++ < 200) fprintf(stderr, "[cd] read $DFFF%02X frame %llu bus=%d\n", off & 0xFE, (unsigned long long)jag->frameCount, jag->busMaster);
    }
    switch (off & 0xFE) {
    case 0x00: return (uint16_t)(ctrl >> 16);
    // The low half reads back the interrupt enables with the live status bits
    // in 9-14, so read-modify-write of BUTCH keeps the master enable.
    case 0x02: return (uint16_t)((ctrl & 0x81FF) | (fifoHalf() ? 1 << 9 : 0) | (respDelay <= 0 && pendingResp.empty() ? 1 << 12 : 0) |
                                 (respReady ? 1 << 13 : 0));
    case 0x04: case 0x06: return dscntrl;
    case 0x0A: {
        if (!respReady || respPos >= resp.size()) return resp.empty() ? 0 : resp.back();
        uint16_t w = resp[respPos++];
        if (respPos >= resp.size()) respReady = false;
        updateIrq();
        return w;
    }
    case 0x10: return 0;
    case 0x12: return (uint16_t)((playing ? 1 : 0) | (i2cntrl & 2) | (i2cntrl & 8) | (fifoCount ? 1 << 4 : 0));
    case 0x14: case 0x16: return sbcntrl;
    case 0x24: case 0x26: case 0x28: case 0x2A: {
        if (!fifoCount) return 0;
        uint32_t l = fifo[fifoR];
        if (!fifoLow) { fifoLow = true; return (uint16_t)(l >> 16); }
        fifoLow = false; fifoR = (fifoR + 1) & 15; fifoCount--;
        updateIrq();
        return (uint16_t)l;
    }
    case 0x2C: return 0;
    case 0x2E: return eeprom.dout ? 8 : 0;   // CD EEPROM data out / ready
    default: return 0;
    }
}

void Butch::write16(uint32_t off, uint16_t v) {
    if (jag->verbose && getenv("JAG_CDLOG") && (off & 0xFE) != 0x0A)
        fprintf(stderr, "[cd] write $DFFF%02X = %04X\n", off & 0xFE, v);
    switch (off & 0xFE) {
    case 0x00: ctrl = (ctrl & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x02: ctrl = (ctrl & 0xFFFF0000u) | v; updateIrq(); break;
    case 0x04: case 0x06: dscntrl = v; break;
    case 0x0A: command(v); break;
    case 0x12: i2cntrl = v; break;
    case 0x16: sbcntrl = v; break;
    case 0x20: sbTime = (sbTime & 0xFFFF) | ((uint32_t)v << 16); break;
    case 0x22: sbTime = (sbTime & 0xFFFF0000u) | v; break;
    case 0x2E: eeprom.write(v); break;
    default: break;
    }
}

void Butch::tick(double sys) {
    if (respDelay > 0) {
        respDelay -= sys;
        if (respDelay <= 0 && !pendingResp.empty()) {
            resp = std::move(pendingResp); pendingResp.clear(); respPos = 0; respReady = true;
        }
    }
    if (seekDelay > 0) {
        seekDelay -= sys;
        if (seekDelay <= 0) {
            playFrame = seekTarget; bytePos = 0; playing = true; fifoClear();
            resp = {0x0100}; respPos = 0; respReady = true;
        }
    }
    sampleAcc += sys;
    double period = jag->clock() / (44100.0 * speed);
    while (sampleAcc >= period) { sampleAcc -= period; produceSample(); }
    updateIrq();
}

void Butch::updateIrq() {
    bool line = (ctrl & 1) && (((ctrl & 2) && fifoHalf()) || ((ctrl & 0x20) && respReady) ||
                               ((ctrl & 0x10) && respDelay <= 0 && pendingResp.empty()));
    if (line && !irqLine) {
        // The cartridge-port interrupt goes through JERRY's external latch: it
        // reaches the GPU as interrupt 1 and the 68000 if JERRY and TOM allow it.
        jag->gpu.raiseInt(1);
        jag->dsp.raiseInt(4);   // JERRY's EXT0 input also interrupts the DSP
        jag->jerryRaise(0);
    }
    irqLine = line;
}

} // namespace jag

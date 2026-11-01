// Jaguar CD: disc images and the "Butch" CD controller.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>

namespace jag {

class Jaguar;

// A CD image: Redump-style .cue with one .bin per track and REM SESSION markers.
// Positions are absolute CD frames (75 per second) counted from 00:00:00, so the
// first track's audio starts at frame 150 (00:02:00).
struct CdImage {
    struct Track {
        int number = 0, session = 1;
        int start = 0;          // absolute frame of INDEX 01
        int fileStart = 0;      // absolute frame of the first sector in its file
        int frames = 0;         // sectors in the file
        bool data = false;      // MODE1/MODE2 track (Jaguar discs use audio tracks)
        std::string path;
        FILE* fp = nullptr;     // kept open for the life of the image
        long base = 0;          // where the track's data starts in fp (non-zero inside a zip)
    };
    struct Session { int firstTrack = 0, lastTrack = 0, leadout = 0; };
    std::vector<Track> tracks;
    std::vector<Session> sessions;

    // Opens a track file named in the cue sheet. The data may sit inside a
    // larger file (a stored entry in a zip): base is its offset and size its
    // length (-1 = to the end of the file). fp == nullptr means not found.
    struct Opened { FILE* fp = nullptr; long base = 0; long size = -1; };
    using Opener = std::function<Opened(const std::string& name)>;
    bool load(const std::string& cuePath, std::string* err);
    bool loadText(const std::string& cueText, const Opener& open, std::string* err);
    // A zipped disc: the cue sheet and its tracks inside one zip. Stored tracks
    // are read in place; compressed ones are unpacked into cacheDir first
    // (progress 0..1, return false to cancel). openZip opens a fresh handle.
    bool loadZip(const std::function<FILE*()>& openZip, const std::string& cacheDir,
                 const std::function<bool(double)>& progress, std::string* err);
    // Reads the 2352-byte raw sector at an absolute frame; zeros in gaps.
    void readSector(int frame, uint8_t* out);
    int trackAt(int frame) const;    // index into tracks, or -1
    ~CdImage();

};

// The CD unit's own serial EEPROM (93C14 family: 64 x 16-bit words), driven by
// software through bit 0 = chip select (active low), bit 1 = clock, bit 2 = data in, with the
// chip's data out on bit 3. Commands are a start bit, 2-bit opcode, 6-bit address.
struct SerialEeprom {
    uint16_t mem[64];
    bool cs = false, clk = false, dout = true, writeEnable = false;
    int state = 0, bits = 0, op = 0, addr = 0;
    uint32_t shift = 0;
    SerialEeprom() { for (auto& w : mem) w = 0xFFFF; }
    void write(uint16_t v);        // new pin levels
};

// Butch: the CD unit's interface chip at $DFFF00. It passes drive (DSA) commands
// and responses, streams disc data into a small FIFO for the GPU, and raises the
// cartridge-port interrupt that reaches the GPU through JERRY.
struct Butch {
    Jaguar* jag = nullptr;
    CdImage* disc = nullptr;

    uint32_t ctrl = 0;        // BUTCH: interrupt enables (bit0 master, 1 FIFO, 4 tx empty, 5 rx full)
    uint16_t i2cntrl = 0, sbcntrl = 0, dscntrl = 0;
    SerialEeprom eeprom;
    uint32_t sbTime = 0;

    // DSA command/response channel
    std::vector<uint16_t> resp;   // queued response words
    size_t respPos = 0;
    double respDelay = 0;         // system clocks until the response arrives
    bool respReady = false;
    std::vector<uint16_t> pendingResp;   // filled when respDelay runs out
    int gotoMin = 0, gotoSec = 0;
    int speed = 2;                // 1 single, 2 double
    int lastSession = 0;

    // Disc transport
    bool playing = false;
    int playFrame = 0;            // absolute frame being streamed
    int bytePos = 0;              // next byte within the sector
    uint8_t sector[2352];
    int sectorFrame = -1;
    double seekDelay = 0;         // clocks until a seek completes
    int seekTarget = 0;

    // The drive clocks out one 4-byte stereo frame per I2S word-clock period
    // (44.1 kHz per speed step). Each one goes to JERRY's I2S receiver and,
    // when enabled, into Butch's 16-long FIFO for the GPU to read.
    uint32_t fifo[16];
    int fifoR = 0, fifoCount = 0;
    bool fifoLow = false;         // next 16-bit read takes the low half of the head long
    double sampleAcc = 0;
    uint16_t fifoPrev = 0;        // right word of the previous frame (see produceSample)
    bool fifoHalf() const { return fifoCount >= 8; }
    bool irqLine = false;
    uint64_t dataWords = 0, fifoOverflows = 0;       // debug: words delivered

    void reset();
    uint16_t read16(uint32_t off);
    void write16(uint32_t off, uint16_t v);
    void tick(double sysClocks);
    void command(uint16_t cmd);
    void respond(std::vector<uint16_t> words, double delay);
    uint16_t nextDataWord();
    void produceSample();
    void fifoClear() { fifoR = fifoCount = 0; fifoLow = false; }
    void updateIrq();
    void debugDump();
};

} // namespace jag

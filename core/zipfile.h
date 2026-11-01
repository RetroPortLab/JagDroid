// Minimal zip reader: central directory, stored and deflated entries, zip64 sizes.
// Used for zipped cartridges and zipped Jaguar CD images.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace jag {

struct ZipEntry {
    std::string name;          // full path inside the archive
    uint16_t method = 0;       // 0 stored, 8 deflated
    uint64_t csize = 0, usize = 0, localOffset = 0;
    std::string baseName() const;   // name without any folder part
};

class ZipArchive {
public:
    // Reads the central directory from an open file (not owned).
    bool open(FILE* f, std::string* err);
    const std::vector<ZipEntry>& entries() const { return list; }
    const ZipEntry* find(const std::string& baseName) const;   // case-insensitive, ignores folders
    // Offset of the entry's data in the archive (after its local header).
    bool dataOffset(const ZipEntry& e, uint64_t& off, std::string* err) const;
    bool readAll(const ZipEntry& e, std::vector<uint8_t>& out, std::string* err) const;
    // Decompresses an entry to a file; progress gets 0..1. Returns false on error or cancel.
    bool extractTo(const ZipEntry& e, const std::string& path, const std::function<bool(double)>& progress,
                   std::string* err) const;

private:
    FILE* fp = nullptr;
    std::vector<ZipEntry> list;
    bool stream(const ZipEntry& e, const std::function<bool(const uint8_t*, size_t)>& sink,
                const std::function<bool(double)>& progress, std::string* err) const;
};

// The first cartridge image (.j64 / .jag / .rom) in a zip, read into memory.
bool readCartFromZip(FILE* f, std::vector<uint8_t>& out, std::string* entryName, std::string* err);

// Helpers for file names.
bool endsWithNoCase(const std::string& s, const char* suffix);

} // namespace jag

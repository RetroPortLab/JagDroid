#include "zipfile.h"
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace jag {

namespace {
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t* p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

bool seekTo(FILE* f, uint64_t off) {
#if defined(_WIN32)
    return _fseeki64(f, (long long)off, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)off, SEEK_SET) == 0;
#endif
}
uint64_t fileSize(FILE* f) {
#if defined(_WIN32)
    _fseeki64(f, 0, SEEK_END); return (uint64_t)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END); return (uint64_t)ftello(f);
#endif
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
} // namespace

bool endsWithNoCase(const std::string& s, const char* suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && lower(s.substr(s.size() - n)) == lower(suffix);
}

std::string ZipEntry::baseName() const {
    size_t p = name.find_last_of("/\\");
    return p == std::string::npos ? name : name.substr(p + 1);
}

bool ZipArchive::open(FILE* f, std::string* err) {
    fp = f; list.clear();
    uint64_t size = fileSize(f);
    if (size < 22) { if (err) *err = "Not a zip file"; return false; }
    // The end-of-central-directory record is in the last 64 KB + 22 bytes.
    uint64_t tail = std::min<uint64_t>(size, 65536 + 22);
    std::vector<uint8_t> buf(tail);
    if (!seekTo(f, size - tail) || fread(buf.data(), 1, tail, f) != tail) { if (err) *err = "Can't read zip"; return false; }
    long eocd = -1;
    for (long i = (long)tail - 22; i >= 0; i--) if (le32(&buf[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { if (err) *err = "Not a zip file"; return false; }
    uint64_t count = le16(&buf[eocd + 10]);
    uint64_t cdSize = le32(&buf[eocd + 12]);
    uint64_t cdOff = le32(&buf[eocd + 16]);
    // Zip64: a locator sits just before the EOCD.
    if ((cdOff == 0xFFFFFFFFu || count == 0xFFFF) && eocd >= 20 && le32(&buf[eocd - 20]) == 0x07064b50) {
        uint64_t z64 = le64(&buf[eocd - 20 + 8]);
        uint8_t r[56];
        if (seekTo(f, z64) && fread(r, 1, 56, f) == 56 && le32(r) == 0x06064b50) {
            count = le64(r + 32); cdSize = le64(r + 40); cdOff = le64(r + 48);
        }
    }
    if (cdSize > 64u * 1024 * 1024) { if (err) *err = "Zip directory too large"; return false; }
    std::vector<uint8_t> cd(cdSize);
    if (!seekTo(f, cdOff) || fread(cd.data(), 1, cdSize, f) != cdSize) { if (err) *err = "Can't read zip directory"; return false; }
    size_t p = 0;
    for (uint64_t i = 0; i < count && p + 46 <= cd.size(); i++) {
        if (le32(&cd[p]) != 0x02014b50) break;
        ZipEntry e;
        e.method = le16(&cd[p + 10]);
        e.csize = le32(&cd[p + 20]); e.usize = le32(&cd[p + 24]);
        uint16_t nlen = le16(&cd[p + 28]), xlen = le16(&cd[p + 30]), clen = le16(&cd[p + 32]);
        e.localOffset = le32(&cd[p + 42]);
        if (p + 46 + nlen + xlen > cd.size()) break;
        e.name.assign((const char*)&cd[p + 46], nlen);
        // Zip64 extra field carries the full sizes / offset for big entries.
        size_t x = p + 46 + nlen, xe = x + xlen;
        while (x + 4 <= xe) {
            uint16_t id = le16(&cd[x]), len = le16(&cd[x + 2]);
            if (id == 0x0001) {
                size_t q = x + 4;
                if (e.usize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.usize = le64(&cd[q]); q += 8; }
                if (e.csize == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.csize = le64(&cd[q]); q += 8; }
                if (e.localOffset == 0xFFFFFFFFu && q + 8 <= x + 4 + len) { e.localOffset = le64(&cd[q]); q += 8; }
            }
            x += 4 + len;
        }
        if (!e.name.empty() && e.name.back() != '/') list.push_back(e);
        p += 46 + nlen + xlen + clen;
    }
    return true;
}

const ZipEntry* ZipArchive::find(const std::string& baseName) const {
    std::string want = lower(baseName);
    size_t s = want.find_last_of("/\\");
    if (s != std::string::npos) want = want.substr(s + 1);
    for (auto& e : list) if (lower(e.baseName()) == want) return &e;
    return nullptr;
}

bool ZipArchive::dataOffset(const ZipEntry& e, uint64_t& off, std::string* err) const {
    uint8_t h[30];
    if (!seekTo(fp, e.localOffset) || fread(h, 1, 30, fp) != 30 || le32(h) != 0x04034b50) {
        if (err) *err = "Damaged zip entry: " + e.name; return false;
    }
    off = e.localOffset + 30 + le16(h + 26) + le16(h + 28);
    return true;
}

bool ZipArchive::stream(const ZipEntry& e, const std::function<bool(const uint8_t*, size_t)>& sink,
                        const std::function<bool(double)>& progress, std::string* err) const {
    uint64_t off;
    if (!dataOffset(e, off, err) || !seekTo(fp, off)) return false;
    std::vector<uint8_t> in(1 << 18), out(1 << 18);
    uint64_t left = e.csize, done = 0;
    if (e.method == 0) {
        while (left) {
            size_t n = (size_t)std::min<uint64_t>(left, in.size());
            if (fread(in.data(), 1, n, fp) != n) { if (err) *err = "Zip is truncated"; return false; }
            if (!sink(in.data(), n)) return false;
            left -= n; done += n;
            if (progress && !progress((double)done / std::max<uint64_t>(1, e.csize))) { if (err) *err = "Cancelled"; return false; }
        }
        return true;
    }
    if (e.method != 8) { if (err) *err = "Unsupported zip compression in " + e.name; return false; }
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) { if (err) *err = "zlib error"; return false; }
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        if (z.avail_in == 0) {
            if (!left) break;
            size_t n = (size_t)std::min<uint64_t>(left, in.size());
            if (fread(in.data(), 1, n, fp) != n) { inflateEnd(&z); if (err) *err = "Zip is truncated"; return false; }
            z.next_in = in.data(); z.avail_in = (uInt)n; left -= n; done += n;
            if (progress && !progress((double)done / std::max<uint64_t>(1, e.csize))) { inflateEnd(&z); if (err) *err = "Cancelled"; return false; }
        }
        z.next_out = out.data(); z.avail_out = (uInt)out.size();
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); if (err) *err = "Damaged zip data in " + e.name; return false; }
        size_t got = out.size() - z.avail_out;
        if (got && !sink(out.data(), got)) { inflateEnd(&z); return false; }
    }
    inflateEnd(&z);
    return true;
}

bool ZipArchive::readAll(const ZipEntry& e, std::vector<uint8_t>& out, std::string* err) const {
    if (e.usize > 512u * 1024 * 1024) { if (err) *err = "Zip entry too large"; return false; }
    out.clear(); out.reserve((size_t)e.usize);
    return stream(e, [&](const uint8_t* d, size_t n) { out.insert(out.end(), d, d + n); return true; }, nullptr, err);
}

bool ZipArchive::extractTo(const ZipEntry& e, const std::string& path, const std::function<bool(double)>& progress,
                           std::string* err) const {
    std::string tmp = path + ".part";
    FILE* o = fopen(tmp.c_str(), "wb");
    if (!o) { if (err) *err = "Can't write " + path; return false; }
    bool ok = stream(e, [&](const uint8_t* d, size_t n) {
        if (fwrite(d, 1, n, o) != n) { if (err) *err = "Not enough storage space to unpack the disc"; return false; }
        return true;
    }, progress, err);
    ok = (fclose(o) == 0) && ok;
    if (ok) ok = rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) remove(tmp.c_str());
    return ok;
}

bool readCartFromZip(FILE* f, std::vector<uint8_t>& out, std::string* entryName, std::string* err) {
    ZipArchive zip;
    if (!zip.open(f, err)) return false;
    for (auto& e : zip.entries()) {
        if (endsWithNoCase(e.name, ".j64") || endsWithNoCase(e.name, ".jag") || endsWithNoCase(e.name, ".rom")) {
            if (entryName) *entryName = e.baseName();
            return zip.readAll(e, out, err);
        }
    }
    if (err) *err = "No Jaguar cartridge (.j64, .jag or .rom) in the zip";
    return false;
}

} // namespace jag

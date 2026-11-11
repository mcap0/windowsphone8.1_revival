// SPDX-License-Identifier: GPL-3.0-or-later
#include "Mp4Tags.h"
#include "TitleCleaner.h"

#include <cstdio>
#include <cstring>

namespace perle {
namespace {

typedef std::vector<uint8_t> Bytes;

const uint64_t MaxMoovBytes = 64ull * 1024 * 1024;

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]); }
uint64_t Be64(const uint8_t* p) { return (uint64_t(Be32(p)) << 32) | Be32(p + 4); }

void Put32(Bytes& b, uint32_t v)
{
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

void PutType(Bytes& b, const char* type) { b.insert(b.end(), type, type + 4); }
void Append(Bytes& b, const Bytes& x) { b.insert(b.end(), x.begin(), x.end()); }

Bytes Box(const char* type, const Bytes& payload)
{
    Bytes b;
    Put32(b, static_cast<uint32_t>(8 + payload.size()));
    PutType(b, type);
    Append(b, payload);
    return b;
}

// "data" atom: type indicator (1 = UTF-8, 13 = JPEG, 14 = PNG), locale 0, payload.
Bytes DataAtom(uint32_t typeIndicator, const Bytes& payload)
{
    Bytes p;
    Put32(p, typeIndicator);
    Put32(p, 0);
    Append(p, payload);
    return Box("data", p);
}

Bytes TextItem(const char* itemType, const std::string& text)
{
    return Box(itemType, DataAtom(1, Bytes(text.begin(), text.end())));
}

struct Atom
{
    uint64_t start;     // offset of the box header
    uint64_t size;      // whole box, header included
    uint64_t header;    // 8 or 16
    char type[5];
};

bool ParseHeader(const uint8_t* buf, uint64_t avail, uint64_t start, uint64_t total, Atom& a)
{
    if (avail < 8) return false;
    uint64_t size = Be32(buf);
    a.start = start;
    a.header = 8;
    std::memcpy(a.type, buf + 4, 4);
    a.type[4] = 0;
    if (size == 1)
    {
        if (avail < 16) return false;
        size = Be64(buf + 8);
        a.header = 16;
    }
    else if (size == 0)
    {
        size = total - start;
    }
    if (size < a.header) return false;
    a.size = size;
    return true;
}

// Children of a container held in memory: [begin, end) of buf.
std::vector<Atom> Children(const Bytes& buf, uint64_t begin, uint64_t end)
{
    std::vector<Atom> out;
    uint64_t pos = begin;
    while (pos + 8 <= end)
    {
        Atom a;
        if (!ParseHeader(buf.data() + pos, end - pos, pos, end, a)) break;
        if (pos + a.size > end) break;
        out.push_back(a);
        pos += a.size;
    }
    return out;
}

bool SameType(const Atom& a, const char* t) { return std::strcmp(a.type, t) == 0; }

// Adds delta to every chunk offset that is at or after threshold (the data that moved with the moov box).
void PatchOffsets(Bytes& buf, uint64_t begin, uint64_t end, int64_t delta, uint64_t threshold)
{
    for (const Atom& a : Children(buf, begin, end))
    {
        uint64_t payload = a.start + a.header;
        uint64_t stop = a.start + a.size;
        if (SameType(a, "trak") || SameType(a, "mdia") || SameType(a, "minf") || SameType(a, "stbl"))
        {
            PatchOffsets(buf, payload, stop, delta, threshold);
        }
        else if (SameType(a, "stco") || SameType(a, "co64"))
        {
            bool is64 = SameType(a, "co64");
            uint64_t count = Be32(&buf[payload + 4]);
            uint64_t entry = is64 ? 8 : 4;
            for (uint64_t i = 0; i < count; i++)
            {
                uint64_t at = payload + 8 + i * entry;
                if (at + entry > stop) break;
                uint8_t* p = &buf[at];
                uint64_t v = is64 ? Be64(p) : Be32(p);
                if (v < threshold) continue;
                v = static_cast<uint64_t>(static_cast<int64_t>(v) + delta);
                if (is64)
                {
                    for (int k = 0; k < 8; k++) p[k] = static_cast<uint8_t>(v >> (56 - 8 * k));
                }
                else
                {
                    for (int k = 0; k < 4; k++) p[k] = static_cast<uint8_t>(v >> (24 - 8 * k));
                }
            }
        }
    }
}

Bytes Hdlr()
{
    Bytes p;
    Put32(p, 0);                 // version and flags
    Put32(p, 0);                 // pre_defined
    PutType(p, "mdir");          // handler type: iTunes metadata
    PutType(p, "appl");          // manufacturer
    Put32(p, 0); Put32(p, 0); Put32(p, 0);
    p.push_back(0);              // empty name
    return Box("hdlr", p);
}

Bytes BuildUdta(const Mp4Tags& t)
{
    Bytes ilst;
    if (!t.title.empty()) Append(ilst, TextItem("\xA9nam", t.title));
    if (!t.artist.empty()) Append(ilst, TextItem("\xA9" "ART", t.artist));
    if (!t.album.empty()) Append(ilst, TextItem("\xA9" "alb", t.album));
    if (!t.year.empty()) Append(ilst, TextItem("\xA9" "day", t.year));
    if (!t.cover.empty())
    {
        bool png = t.cover.size() > 4 && t.cover[0] == 0x89 && t.cover[1] == 'P' && t.cover[2] == 'N' && t.cover[3] == 'G';
        Append(ilst, Box("covr", DataAtom(png ? 14 : 13, t.cover)));
    }

    Bytes meta;
    Put32(meta, 0);              // FullBox version and flags
    Append(meta, Hdlr());
    Append(meta, Box("ilst", ilst));
    return Box("udta", Box("meta", meta));
}

std::string ReadError(const char* what) { return std::string(what); }

bool ReadWhole(FILE* f, uint64_t pos, uint64_t n, Bytes& out)
{
    out.resize(static_cast<size_t>(n));
    if (_fseeki64(f, static_cast<__int64>(pos), SEEK_SET) != 0) return false;
    return n == 0 || std::fread(&out[0], 1, static_cast<size_t>(n), f) == n;
}

bool CopyRange(FILE* in, FILE* out, uint64_t from, uint64_t to)
{
    if (_fseeki64(in, static_cast<__int64>(from), SEEK_SET) != 0) return false;
    Bytes chunk(1 << 16);
    uint64_t left = to - from;
    while (left > 0)
    {
        size_t want = static_cast<size_t>(left < chunk.size() ? left : chunk.size());
        if (std::fread(&chunk[0], 1, want, in) != want) return false;
        if (std::fwrite(&chunk[0], 1, want, out) != want) return false;
        left -= want;
    }
    return true;
}

// Top-level boxes of the file, read from their headers only.
bool TopLevel(FILE* f, uint64_t fileSize, std::vector<Atom>& out)
{
    uint64_t pos = 0;
    while (pos + 8 <= fileSize)
    {
        Bytes head;
        if (!ReadWhole(f, pos, 16 < fileSize - pos ? 16 : fileSize - pos, head)) return false;
        Atom a;
        if (!ParseHeader(head.data(), head.size(), pos, fileSize, a)) return false;
        if (pos + a.size > fileSize) return false;
        out.push_back(a);
        pos += a.size;
    }
    return true;
}

uint64_t FileSize(FILE* f)
{
    _fseeki64(f, 0, SEEK_END);
    return static_cast<uint64_t>(_ftelli64(f));
}

}  // namespace

std::string WriteMp4Tags(const std::wstring& src, const std::wstring& dst, const Mp4Tags& tags)
{
    FILE* in = _wfopen(src.c_str(), L"rb");
    if (!in) return ReadError("Impossibile aprire il file scaricato");
    FILE* out = _wfopen(dst.c_str(), L"wb");
    if (!out) { std::fclose(in); return ReadError("Impossibile creare il file con i tag"); }

    std::string error;
    do
    {
        uint64_t size = FileSize(in);
        std::vector<Atom> top;
        if (!TopLevel(in, size, top)) { error = "File MP4 non valido"; break; }

        const Atom* moov = nullptr;
        for (const Atom& a : top) if (SameType(a, "moov")) moov = &a;
        if (!moov) { error = "Nessun box moov nel file"; break; }
        if (moov->size > MaxMoovBytes) { error = "Box moov troppo grande"; break; }

        Bytes old;
        if (!ReadWhole(in, moov->start, moov->size, old)) { error = "Lettura del box moov non riuscita"; break; }

        // keep every child except the old udta, then append the new one
        Bytes kept;
        for (const Atom& a : Children(old, moov->header, moov->size))
        {
            if (SameType(a, "udta")) continue;
            kept.insert(kept.end(), old.begin() + a.start, old.begin() + a.start + a.size);
        }
        Append(kept, BuildUdta(tags));

        Bytes newMoov;
        Put32(newMoov, static_cast<uint32_t>(8 + kept.size()));
        PutType(newMoov, "moov");
        Append(newMoov, kept);

        // the data after the old moov moves by delta: fix the chunk offsets of the sample tables
        int64_t delta = static_cast<int64_t>(newMoov.size()) - static_cast<int64_t>(moov->size);
        uint64_t oldEnd = moov->start + moov->size;
        if (delta != 0)
        {
            Bytes body(kept.begin(), kept.end());
            // PatchOffsets works on a buffer with the children at [0, kept.size())
            PatchOffsets(body, 0, body.size(), delta, oldEnd);
            Bytes rebuilt;
            Put32(rebuilt, static_cast<uint32_t>(8 + body.size()));
            PutType(rebuilt, "moov");
            Append(rebuilt, body);
            newMoov.swap(rebuilt);
        }

        if (!CopyRange(in, out, 0, moov->start)) { error = "Scrittura non riuscita"; break; }
        if (std::fwrite(&newMoov[0], 1, newMoov.size(), out) != newMoov.size()) { error = "Scrittura non riuscita"; break; }
        if (!CopyRange(in, out, oldEnd, size)) { error = "Scrittura non riuscita"; break; }
    } while (false);

    std::fclose(in);
    if (std::fclose(out) != 0 && error.empty()) error = "Chiusura del file non riuscita";
    return error;
}

bool ReadMp4Tags(const std::wstring& path, Mp4Tags& out, std::string& error)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { error = "Impossibile aprire il file"; return false; }
    bool ok = false;
    do
    {
        uint64_t size = FileSize(f);
        std::vector<Atom> top;
        if (!TopLevel(f, size, top)) { error = "File MP4 non valido"; break; }
        const Atom* moov = nullptr;
        for (const Atom& a : top) if (SameType(a, "moov")) moov = &a;
        if (!moov) { error = "Nessun box moov nel file"; break; }
        Bytes buf;
        if (!ReadWhole(f, moov->start, moov->size, buf)) { error = "Lettura non riuscita"; break; }

        for (const Atom& udta : Children(buf, moov->header, moov->size))
        {
            if (!SameType(udta, "udta")) continue;
            for (const Atom& meta : Children(buf, udta.start + udta.header, udta.start + udta.size))
            {
                if (!SameType(meta, "meta")) continue;
                uint64_t metaBegin = meta.start + meta.header + 4;  // FullBox
                for (const Atom& ilst : Children(buf, metaBegin, meta.start + meta.size))
                {
                    if (!SameType(ilst, "ilst")) continue;
                    for (const Atom& item : Children(buf, ilst.start + ilst.header, ilst.start + ilst.size))
                    {
                        for (const Atom& data : Children(buf, item.start + item.header, item.start + item.size))
                        {
                            if (!SameType(data, "data") || data.size < data.header + 8) continue;
                            uint64_t p = data.start + data.header + 8;  // type and locale
                            Bytes value(buf.begin() + p, buf.begin() + data.start + data.size);
                            std::string text(value.begin(), value.end());
                            const uint8_t* t = reinterpret_cast<const uint8_t*>(item.type);
                            if (t[0] == 0xA9 && t[1] == 'n') out.title = text;
                            else if (t[0] == 0xA9 && t[1] == 'A') out.artist = text;
                            else if (t[0] == 0xA9 && t[1] == 'a') out.album = text;
                            else if (t[0] == 0xA9 && t[1] == 'd') out.year = text;
                            else if (std::strncmp(item.type, "covr", 4) == 0) out.cover = value;
                        }
                    }
                }
            }
        }
        ok = true;
    } while (false);
    std::fclose(f);
    return ok;
}

}  // namespace perle

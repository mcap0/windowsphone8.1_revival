// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the Perle 8.1 native code (no WinRT): title cleaning and MP4 tags round trip.
// Usage: Perle81.Native.Tests.exe [input.m4a output.m4a]
//   With two arguments it also writes the tags into a real file, to be checked with ffprobe.
#include "../Mp4Tags.h"
#include "../TitleCleaner.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace perle;

static int failures = 0;

static void Check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

static void CheckTitle(const wchar_t* in, const wchar_t* expected)
{
    std::wstring got = CleanTitle(in);
    Check(got == expected, ToUtf8(std::wstring(in)) + " -> \"" + ToUtf8(got) + "\"");
}

static void TitleTests()
{
    std::printf("CleanTitle\n");
    CheckTitle(L"Song Name (Official Video)", L"Song Name");
    CheckTitle(L"Song Name [Lyrics]", L"Song Name");
    CheckTitle(L"Song Name (Official Music Video) HD", L"Song Name");
    CheckTitle(L"Song Name 4K", L"Song Name");
    CheckTitle(L"Song (feat. Guest)", L"Song (feat. Guest)");
    CheckTitle(L"Artist - Song (Audio)", L"Artist - Song");
    CheckTitle(L"  Song   Title  -  ", L"Song Title");
    CheckTitle(L"Song (Remix)", L"Song (Remix)");

    std::printf("ArtistOf / TrackOf\n");
    Check(ArtistOf(L"Channel", L"Artista - Brano (Official Video)") == L"Artista", "artist from \"Artista - Brano\"");
    Check(TrackOf(L"Channel", L"Artista - Brano (Official Video)") == L"Brano", "track from \"Artista - Brano\"");
    Check(ArtistOf(L"Channel Name", L"Brano senza trattino") == L"Channel Name", "artist = channel");
    Check(ArtistOf(L"Queen - Topic", L"Bohemian Rhapsody") == L"Queen", "channel without \" - Topic\"");
    Check(ArtistOf(L"ArtistVEVO", L"Title") == L"Artist", "channel without VEVO");
    Check(TrackOf(L"Channel", L"Brano senza trattino") == L"Brano senza trattino", "track = cleaned title");
    Check(SafeFileName(L"AC/DC: Live?") == L"AC_DC_ Live_", "file name without forbidden characters");
}

// ---- synthetic MP4 ---------------------------------------------------------------------------

typedef std::vector<unsigned char> Bytes;

static void Append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }

static void Put32(Bytes& b, unsigned v)
{
    b.push_back(static_cast<unsigned char>(v >> 24)); b.push_back(static_cast<unsigned char>(v >> 16));
    b.push_back(static_cast<unsigned char>(v >> 8)); b.push_back(static_cast<unsigned char>(v));
}

static Bytes Box(const char* type, const Bytes& payload)
{
    Bytes b;
    Put32(b, static_cast<unsigned>(8 + payload.size()));
    b.insert(b.end(), type, type + 4);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

static const unsigned SampleBytes = 1000;

static Bytes Samples()
{
    Bytes s(SampleBytes);
    for (unsigned i = 0; i < SampleBytes; i++) s[i] = static_cast<unsigned char>((i * 7 + 3) & 0xFF);
    return s;
}

// moov with one trak whose stco points at offset.
static Bytes Moov(unsigned offset)
{
    Bytes stco;
    Put32(stco, 0);   // version and flags
    Put32(stco, 1);   // entry count
    Put32(stco, offset);
    Bytes stbl = Box("stco", stco);
    Bytes minf = Box("minf", Box("stbl", stbl));
    Bytes mdia = Box("mdia", minf);
    Bytes trak = Box("trak", mdia);
    return Box("moov", trak);
}

static Bytes Ftyp()
{
    Bytes p;
    p.insert(p.end(), {'M', '4', 'A', ' '});
    Put32(p, 0);
    p.insert(p.end(), {'M', '4', 'A', ' ', 'm', 'p', '4', '2'});
    return Box("ftyp", p);
}

static bool WriteFile(const std::wstring& path, const Bytes& data)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

static Bytes ReadAll(const std::wstring& path)
{
    Bytes out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    unsigned char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0) out.insert(out.end(), chunk, chunk + n);
    std::fclose(f);
    return out;
}

// Offset stored in the first stco entry of the file (the test file has no other stco).
static unsigned StcoOffset(const Bytes& file)
{
    for (size_t i = 0; i + 4 <= file.size(); i++)
        if (std::memcmp(&file[i], "stco", 4) == 0)
            return (unsigned(file[i + 12]) << 24) | (unsigned(file[i + 13]) << 16) | (unsigned(file[i + 14]) << 8) | file[i + 15];
    return 0xFFFFFFFF;
}

static bool SamplesAt(const Bytes& file, unsigned offset)
{
    Bytes want = Samples();
    return offset + SampleBytes <= file.size() && std::memcmp(&file[offset], want.data(), SampleBytes) == 0;
}

static Mp4Tags SampleTags()
{
    Mp4Tags t;
    t.title = ToUtf8(L"Titolo con \u00e0 e \u20ac");
    t.artist = "Artista";
    t.album = "Perle";
    t.year = "2026";
    t.cover = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 0xFF, 0xD9};
    return t;
}

static void RoundTrip(bool moovFirst)
{
    std::printf("MP4 round trip (%s)\n", moovFirst ? "moov before mdat" : "moov after mdat");
    Bytes ftyp = Ftyp();
    Bytes mdatPayload = Samples();
    Bytes mdat = Box("mdat", mdatPayload);
    Bytes file;
    if (moovFirst)
    {
        unsigned moovSize = static_cast<unsigned>(Moov(0).size());
        unsigned dataAt = static_cast<unsigned>(ftyp.size() + moovSize + 8);
        file = ftyp;
        Append(file, Moov(dataAt));
        Append(file, mdat);
    }
    else
    {
        unsigned dataAt = static_cast<unsigned>(ftyp.size() + 8);
        file = ftyp;
        Append(file, mdat);
        Append(file, Moov(dataAt));
    }
    std::wstring src = L"roundtrip_src.m4a", dst = L"roundtrip_dst.m4a";
    Check(WriteFile(src, file), "synthetic file written");
    Check(SamplesAt(file, StcoOffset(file)), "input offset points at the samples");

    std::string err = WriteMp4Tags(src, dst, SampleTags());
    Check(err.empty(), "tags written" + (err.empty() ? std::string() : " (" + err + ")"));
    Bytes out = ReadAll(dst);
    Check(SamplesAt(out, StcoOffset(out)), "offset moved with the moov box, samples still found");

    Mp4Tags back;
    std::string readErr;
    Check(ReadMp4Tags(dst, back, readErr), "tags read back");
    Check(back.title == SampleTags().title, "title");
    Check(back.artist == "Artista", "artist");
    Check(back.album == "Perle", "album");
    Check(back.year == "2026", "year");
    Check(back.cover == SampleTags().cover, "cover bytes");

    // a second write replaces the tags instead of adding a second udta
    Mp4Tags again;
    again.title = "Altro";
    Check(WriteMp4Tags(dst, src, again).empty(), "second write");
    Mp4Tags second;
    Check(ReadMp4Tags(src, second, readErr) && second.title == "Altro" && second.artist.empty(), "second write replaces the tags");
    Check(SamplesAt(ReadAll(src), StcoOffset(ReadAll(src))), "offset still valid after the second write");
}

int wmain(int argc, wchar_t** argv)
{
    TitleTests();
    RoundTrip(false);
    RoundTrip(true);

    if (argc == 3)
    {
        std::printf("Writing tags into a real file: %ls -> %ls\n", argv[1], argv[2]);
        std::string err = WriteMp4Tags(argv[1], argv[2], SampleTags());
        Check(err.empty(), "real file tagged" + (err.empty() ? std::string() : " (" + err + ")"));
    }

    std::printf("%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}

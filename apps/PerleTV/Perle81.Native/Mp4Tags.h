// SPDX-License-Identifier: GPL-3.0-or-later
// MP4/M4A iTunes-style tags (moov/udta/meta/ilst): writer and reader (pure C++, no WinRT).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace perle {

struct Mp4Tags
{
    std::string title;   // ©nam, UTF-8
    std::string artist;  // ©ART
    std::string album;   // ©alb
    std::string albumArtist;  // aART
    std::string year;    // ©day
    unsigned int track;       // trkn: position in the album, 0 = no number
    unsigned int trackTotal;  // trkn: tracks in the album, 0 = unknown
    std::vector<uint8_t> cover;  // covr: JPEG (or PNG when it starts with the PNG signature)
    Mp4Tags() : track(0), trackTotal(0) {}
};

// Copies src to dst with the tags written in moov/udta (an existing udta is replaced).
// Chunk offsets (stco/co64) are moved when the moov box changes size and data follows it.
// Returns an empty string on success, otherwise the error message.
std::string WriteMp4Tags(const std::wstring& src, const std::wstring& dst, const Mp4Tags& tags);

// Reads the tags back. Returns false and sets error when the file is not an MP4 with a moov box.
bool ReadMp4Tags(const std::wstring& path, Mp4Tags& out, std::string& error);

}  // namespace perle

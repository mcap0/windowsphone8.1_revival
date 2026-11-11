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
    std::string year;    // ©day
    std::vector<uint8_t> cover;  // covr: JPEG (or PNG when it starts with the PNG signature)
};

// Copies src to dst with the tags written in moov/udta (an existing udta is replaced).
// Chunk offsets (stco/co64) are moved when the moov box changes size and data follows it.
// Returns an empty string on success, otherwise the error message.
std::string WriteMp4Tags(const std::wstring& src, const std::wstring& dst, const Mp4Tags& tags);

// Reads the tags back. Returns false and sets error when the file is not an MP4 with a moov box.
bool ReadMp4Tags(const std::wstring& path, Mp4Tags& out, std::string& error);

}  // namespace perle

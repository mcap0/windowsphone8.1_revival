// SPDX-License-Identifier: GPL-3.0-or-later
// Windows Runtime component (C++/CX) for Perle 8.1: MP4 tags and title cleaning, called from the C# app.
#pragma once
#include <collection.h>

namespace Perle81 {
namespace Native {

public ref class Mp4TagSet sealed
{
public:
    property Platform::String^ Title;
    property Platform::String^ Artist;
    property Platform::String^ Album;
    property Platform::String^ Year;
    property Platform::Array<unsigned char>^ Cover;
};

public ref class Mp4Tagger sealed
{
public:
    // Writes src to dst with the tags. Returns null on success, otherwise the error message.
    static Platform::String^ Write(Platform::String^ src, Platform::String^ dst, Mp4TagSet^ tags);
};

public ref class TitleTools sealed
{
public:
    static Platform::String^ Clean(Platform::String^ title);
    static Platform::String^ ArtistOf(Platform::String^ channel, Platform::String^ title);
    static Platform::String^ TrackOf(Platform::String^ channel, Platform::String^ title);
    static Platform::String^ SafeName(Platform::String^ name);
};

}  // namespace Native
}  // namespace Perle81

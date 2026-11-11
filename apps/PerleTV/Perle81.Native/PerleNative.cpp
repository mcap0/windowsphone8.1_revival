// SPDX-License-Identifier: GPL-3.0-or-later
#include "PerleNative.h"
#include "Mp4Tags.h"
#include "TitleCleaner.h"

using namespace Perle81::Native;

namespace {

std::wstring W(Platform::String^ s) { return s == nullptr ? std::wstring() : std::wstring(s->Data()); }
Platform::String^ P(const std::wstring& s) { return ref new Platform::String(s.c_str()); }

}  // namespace

Platform::String^ Mp4Tagger::Write(Platform::String^ src, Platform::String^ dst, Mp4TagSet^ tags)
{
    perle::Mp4Tags t;
    if (tags != nullptr)
    {
        t.title = perle::ToUtf8(W(tags->Title));
        t.artist = perle::ToUtf8(W(tags->Artist));
        t.album = perle::ToUtf8(W(tags->Album));
        t.year = perle::ToUtf8(W(tags->Year));
        if (tags->Cover != nullptr)
            t.cover.assign(tags->Cover->Data, tags->Cover->Data + tags->Cover->Length);
    }
    std::string error = perle::WriteMp4Tags(W(src), W(dst), t);
    return error.empty() ? nullptr : P(perle::FromUtf8(error));
}

Platform::String^ TitleTools::Clean(Platform::String^ title) { return P(perle::CleanTitle(W(title))); }

Platform::String^ TitleTools::ArtistOf(Platform::String^ channel, Platform::String^ title)
{
    return P(perle::ArtistOf(W(channel), W(title)));
}

Platform::String^ TitleTools::TrackOf(Platform::String^ channel, Platform::String^ title)
{
    return P(perle::TrackOf(W(channel), W(title)));
}

Platform::String^ TitleTools::SafeName(Platform::String^ name) { return P(perle::SafeFileName(W(name))); }

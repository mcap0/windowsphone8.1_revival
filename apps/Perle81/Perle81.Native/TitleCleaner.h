// SPDX-License-Identifier: GPL-3.0-or-later
// Clean titles of YouTube videos for the tags of "Scarica audio" (pure C++, no WinRT).
#pragma once
#include <string>

namespace perle {

// Removes "(Official Video)", "[Lyrics]", "HD", "4K"... and trims the separators left behind.
std::wstring CleanTitle(const std::wstring& title);

// "Artista - Brano" gives "Artista"; otherwise the channel name (without " - Topic" and " VEVO").
std::wstring ArtistOf(const std::wstring& channel, const std::wstring& title);

// "Artista - Brano" gives "Brano"; otherwise the cleaned title.
std::wstring TrackOf(const std::wstring& channel, const std::wstring& title);

// Characters Windows does not accept in file and folder names are replaced by '_'.
std::wstring SafeFileName(const std::wstring& name);

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

}  // namespace perle

// Songs from the internet for Musica, portable C++11 (tested on the PC: apps/Musica/Tests/online_test.cpp).
// - search: YouTube Music (WEB_REMIX client, "songs" filter): title, artist, album, length, cover
// - audio: the VISIONOS player answer has direct URLs (no JavaScript to run), as Perle TV's "Scarica audio";
//   itag 140 = AAC 128 kb/s in MP4, fetched in ranges of 1 MB (one big request gets throttled)
// - Spotify: the public embed page of a playlist / album / track lists title, artists and length, no account needed
// The app does the HTTP (WinRT HttpClient); this file builds the requests and reads the answers.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace online {

struct Request {
    std::string url;
    std::vector<std::pair<std::string, std::string> > headers;
    std::string body;   // empty = GET
};

struct Song {
    std::string videoId, title, artist, album, thumb;
    int seconds;
    Song() : seconds(0) {}
};

struct Audio {
    std::string url, error;
    long long length;   // bytes, -1 unknown
    Audio() : length(-1) {}
};

struct SpotifyTrack {
    std::string title, artist;   // artists joined with ", "
    std::string album;           // only when the list is an album
    int seconds, number;         // number: position in the album, from 1
    SpotifyTrack() : seconds(0), number(0) {}
};

struct SpotifyList {
    std::string name, kind, error;   // kind: playlist, album, track
    std::string artist, cover;       // album only: its artist, URL of its cover (300 px)
    std::vector<SpotifyTrack> tracks;
};

Request searchRequest(const std::string& query);
bool parseSearch(const std::string& json, std::vector<Song>& out);

Request visitorRequest();
std::string parseVisitor(const std::string& json);
Request playerRequest(const std::string& videoId, const std::string& visitor);
Audio parsePlayer(const std::string& json);
// GET of bytes [from, to] of the audio
Request audioRangeRequest(const Audio& a, long long from, long long to);

// open.spotify.com/{playlist|album|track}/<id>[?...] or spotify:<kind>:<id> -> the embed page; false if not a link
bool spotifyEmbedRequest(const std::string& link, Request& out);
SpotifyList parseSpotifyEmbed(const std::string& html);
// the query to look a Spotify track up on YouTube Music, and the best result for it (-1 if none fits)
std::string queryFor(const SpotifyTrack& t);
int bestMatch(const SpotifyTrack& t, const std::vector<Song>& results);

// Removes the edit lists (moov/trak/edts) of a downloaded MP4 in memory: YouTube's audio has one since late 2026
// and the phone's player does not start such a fragmented file (Perle's older downloads had none and play).
// Box sizes up the chain are fixed; the fragments address their data from their own moof, so nothing else moves.
// Returns how many were removed.
int removeEditLists(std::string& mp4);

// Rewrites a fragmented (DASH) MP4 with one audio track as a plain MP4: the samples of every moof/mdat go into one
// mdat and the sample tables (stts, stsz, stsc, stco) describe them, like any .m4a. WP8.1's player does not start
// YouTube's fragmented files from the music library; the plain ones also seek. Edit lists and mvex are dropped.
// Returns false (mp4 untouched, err set) when the file is not a fragmented MP4 with one track.
bool defragment(std::string& mp4, std::string& err);

// "m:ss"
std::string formatSeconds(int s);
// a name usable as a file name (no \ / : * ? " < > |, no trailing dots/spaces, at most 80 characters)
std::string safeFileName(const std::string& s);

}  // namespace online

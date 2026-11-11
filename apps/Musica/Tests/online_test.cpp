// Live check of Core/Online against YouTube Music, the VISIONOS player and Spotify (HTTP with curl).
// build: g++ -std=c++11 -I../Core -I../../TsNet online_test.cpp ../Core/Online.cpp ../../TsNet/tsjson.cpp -o online_test
// run:   ./online_test "<query>" [spotify-link]
#include "Online.h"
#include <cstdio>
#include <string>

static std::string fetch(const online::Request& r)
{
    std::string cmd = "curl -s --max-time 30 '" + r.url + "'";
    for (size_t i = 0; i < r.headers.size(); ++i) cmd += " -H '" + r.headers[i].first + ": " + r.headers[i].second + "'";
    std::string bodyFile;
    if (!r.body.empty()) {
        bodyFile = "/tmp/online_test_body.json";
        FILE* f = std::fopen(bodyFile.c_str(), "wb");
        std::fwrite(r.body.data(), 1, r.body.size(), f);
        std::fclose(f);
        cmd += " --data-binary @" + bodyFile;
    }
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

int main(int argc, char** argv)
{
    int fails = 0;
    std::vector<online::Song> songs;
    bool ok = online::parseSearch(fetch(online::searchRequest(argc > 1 ? argv[1] : "albachiara vasco")), songs);
    std::printf("%s search: %d songs\n", ok && !songs.empty() ? "ok" : "FAIL", (int)songs.size());
    if (!ok || songs.empty()) return 1;
    for (size_t i = 0; i < songs.size() && i < 3; ++i)
        std::printf("   %s | %s | %s | %s | %s\n", songs[i].videoId.c_str(), songs[i].title.c_str(), songs[i].artist.c_str(),
                    songs[i].album.c_str(), online::formatSeconds(songs[i].seconds).c_str());
    std::string visitor = online::parseVisitor(fetch(online::visitorRequest()));
    std::printf("%s visitor\n", visitor.empty() ? "FAIL" : "ok");
    online::Audio a = online::parsePlayer(fetch(online::playerRequest(songs[0].videoId, visitor)));
    std::printf("%s audio: %lld bytes %s\n", a.url.empty() ? "FAIL" : "ok", a.length, a.error.c_str());
    if (!a.url.empty()) {
        std::string head = fetch(online::audioRangeRequest(a, 0, 1048575));
        bool mp4 = head.size() == 1048576 && head.compare(4, 4, "ftyp") == 0;
        std::printf("%s first MB: %d bytes\n", mp4 ? "ok" : "FAIL", (int)head.size());
        if (!mp4) ++fails;
    } else ++fails;
    if (argc > 2) {
        online::Request r;
        if (!online::spotifyEmbedRequest(argv[2], r)) { std::printf("FAIL spotify link\n"); return 1; }
        online::SpotifyList l = online::parseSpotifyEmbed(fetch(r));
        std::printf("%s spotify %s \"%s\": %d tracks %s\n", l.tracks.empty() ? "FAIL" : "ok", l.kind.c_str(), l.name.c_str(),
                    (int)l.tracks.size(), l.error.c_str());
        if (l.kind == "album") std::printf("   album by \"%s\", cover %s, last track number %d\n", l.artist.c_str(), l.cover.c_str(), l.tracks.back().number);
        int matched = 0;
        for (size_t i = 0; i < l.tracks.size() && i < 8; ++i) {
            std::vector<online::Song> res;
            online::parseSearch(fetch(online::searchRequest(online::queryFor(l.tracks[i]))), res);
            int b = online::bestMatch(l.tracks[i], res);
            if (b >= 0) ++matched;
            std::printf("   %s - %s (%s) -> %s\n", l.tracks[i].artist.c_str(), l.tracks[i].title.c_str(),
                        online::formatSeconds(l.tracks[i].seconds).c_str(),
                        b < 0 ? "NESSUNO" : (res[b].artist + " - " + res[b].title + " (" + online::formatSeconds(res[b].seconds) + ")").c_str());
        }
        std::printf("matched %d of %d\n", matched, (int)(l.tracks.size() < 8 ? l.tracks.size() : 8));
    }
    return fails;
}

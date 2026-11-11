// PC: online::httpsGet (mbedTLS) on the Spotify embed page, the way the phone fetches it.
// build: apps/Musica/Tests/build_https.sh
#include "Https.h"
#include "Online.h"
#include <cstdio>
int main(int argc, char** argv)
{
    online::Request r;
    if (!online::spotifyEmbedRequest(argc > 1 ? argv[1] : "https://open.spotify.com/album/4aawyAB9vmqN3uQ7FjRGTy", r)) return 2;
    std::string err;
    std::string html = online::httpsGet(r.url, r.headers, err);
    online::SpotifyList l = online::parseSpotifyEmbed(html);
    std::printf("%s %d bytes, \"%s\" %d tracks %s %s\n", l.tracks.empty() ? "FAIL" : "ok", (int)html.size(), l.name.c_str(),
                (int)l.tracks.size(), err.c_str(), l.error.c_str());
    return l.tracks.empty();
}

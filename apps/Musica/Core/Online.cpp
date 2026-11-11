#include "Online.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdint.h>

#include "tsjson.h"

namespace online {

namespace {
const char* BrowserUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:153.0) Gecko/20100101 Firefox/153.0";
// same client as Perle.Core.Extractor.Innertube (VISIONOS)
const char* VisionVersion = "1.04";
const char* VisionModel = "RealityDevice17,1";
const char* VisionOs = "26.6.0.23O770";
const char* VisionUa = "com.google.visionos.youtube/1.04(RealityDevice17,1; U; CPU visionOS 26_6_0 like Mac OS X; GB)";
const char* Gapis = "https://youtubei.googleapis.com/youtubei/v1/";

std::string randomString(int n)
{
    static bool seeded = false;
    if (!seeded) { std::srand((unsigned)std::time(0)); seeded = true; }
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    for (int i = 0; i < n; ++i) s += a[std::rand() % 64];
    return s;
}

std::string visionBody(const std::string& visitor, const std::string& extra)
{
    std::string c = "{\"context\":{\"client\":{\"clientName\":\"VISIONOS\",\"clientVersion\":\"" + std::string(VisionVersion) +
        "\",\"clientScreen\":\"WATCH\",\"platform\":\"MOBILE\"";
    if (!visitor.empty()) c += ",\"visitorData\":" + ts::jsonQuote(visitor);
    c += ",\"deviceMake\":\"Apple\",\"deviceModel\":\"" + std::string(VisionModel) + "\",\"osName\":\"visionOS\",\"osVersion\":\"" +
         VisionOs + "\",\"hl\":\"en-GB\",\"gl\":\"GB\",\"utcOffsetMinutes\":0},\"request\":{\"internalExperimentFlags\":[],"
         "\"useSsl\":true},\"user\":{\"lockedSafetyMode\":false}}" + extra + "}";
    return c;
}

void visionHeaders(Request& r)
{
    r.headers.push_back(std::make_pair("User-Agent", std::string(VisionUa)));
    r.headers.push_back(std::make_pair("X-Goog-Api-Format-Version", std::string("2")));
    r.headers.push_back(std::make_pair("Content-Type", std::string("application/json")));
}

std::string runsText(const ts::Json& runs)
{
    std::string s;
    for (size_t i = 0; i < runs.size(); ++i) s += runs[i]["text"].s();
    return s;
}

int parseDuration(const std::string& s)   // "4:06", "1:02:03"
{
    int total = 0, part = 0;
    bool any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') { part = part * 10 + (c - '0'); any = true; }
        else if (c == ':') { total = total * 60 + part; part = 0; }
        else return 0;
    }
    return any ? total * 60 + part : 0;
}

void splitBullets(const std::string& s, std::vector<std::string>& out)
{
    const std::string sep = " \xE2\x80\xA2 ";   // " \xe2\x80\xa2 "
    size_t p = 0;
    while (true) {
        size_t q = s.find(sep, p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + sep.size();
    }
}

void walkSongs(const ts::Json& j, std::vector<Song>& out)
{
    if (j.type == ts::Json::Obj) {
        if (j.has("musicResponsiveListItemRenderer")) {
            const ts::Json& it = j["musicResponsiveListItemRenderer"];
            Song s;
            s.videoId = it["playlistItemData"]["videoId"].s();
            const ts::Json& cols = it["flexColumns"];
            if (cols.size() > 0) s.title = runsText(cols[(size_t)0]["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"]);
            if (cols.size() > 1) {
                std::vector<std::string> parts;
                splitBullets(runsText(cols[(size_t)1]["musicResponsiveListItemFlexColumnRenderer"]["text"]["runs"]), parts);
                // "Artist • Album • 4:06" (songs), sometimes without the album
                if (!parts.empty() && parseDuration(parts.back()) > 0) { s.seconds = parseDuration(parts.back()); parts.pop_back(); }
                if (!parts.empty()) s.artist = parts[0];
                if (parts.size() > 1) s.album = parts[1];
            }
            const ts::Json& th = it["thumbnail"]["musicThumbnailRenderer"]["thumbnail"]["thumbnails"];
            if (th.size() > 0) s.thumb = th[th.size() - 1]["url"].s();
            if (!s.videoId.empty() && !s.title.empty()) out.push_back(s);
            return;
        }
        for (std::map<std::string, ts::Json>::const_iterator i = j.obj.begin(); i != j.obj.end(); ++i) walkSongs(i->second, out);
    } else if (j.type == ts::Json::Arr) {
        for (size_t i = 0; i < j.arr.size(); ++i) walkSongs(j.arr[i], out);
    }
}

std::string lower(const std::string& s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); ++i) r[i] = (char)std::tolower((unsigned char)r[i]);
    return r;
}

// words of a title without the parts that differ between Spotify and YouTube ("feat.", "Remastered", brackets)
std::vector<std::string> words(const std::string& s)
{
    std::string t = lower(s);
    std::vector<std::string> out;
    std::string w;
    int depth = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        char c = i < t.size() ? t[i] : ' ';
        if (c == '(' || c == '[') { ++depth; c = ' '; }
        else if (c == ')' || c == ']') { if (depth) --depth; c = ' '; }
        if (depth) continue;
        if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) w += c;
        else if (!w.empty()) {
            if (w != "feat" && w != "ft" && w != "remastered" && w != "remaster" && w != "version") out.push_back(w);
            w.clear();
        }
    }
    return out;
}

std::string unescapeHtml(const std::string& s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            size_t e = s.find(';', i);
            if (e != std::string::npos && e - i < 8) {
                std::string ent = s.substr(i + 1, e - i - 1);
                const char* rep = ent == "amp" ? "&" : ent == "quot" ? "\"" : ent == "lt" ? "<" : ent == "gt" ? ">" :
                                  ent == "#39" || ent == "#x27" ? "'" : 0;
                if (rep) { r += rep; i = e; continue; }
            }
        }
        r += s[i];
    }
    return r;
}
}  // namespace

Request searchRequest(const std::string& query)
{
    Request r;
    r.url = "https://music.youtube.com/youtubei/v1/search?prettyPrint=false";
    r.body = "{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\",\"clientVersion\":\"1.20260805.01.00\",\"hl\":\"it\","
             "\"gl\":\"IT\"}},\"query\":" + ts::jsonQuote(query) + ",\"params\":\"EgWKAQIIAWoQEAMQBBAJEAoQBRAREBAQFQ%3D%3D\"}";
    r.headers.push_back(std::make_pair("User-Agent", std::string(BrowserUa)));
    r.headers.push_back(std::make_pair("Origin", std::string("https://music.youtube.com")));
    r.headers.push_back(std::make_pair("Referer", std::string("https://music.youtube.com/")));
    r.headers.push_back(std::make_pair("Content-Type", std::string("application/json")));
    return r;
}

bool parseSearch(const std::string& json, std::vector<Song>& out)
{
    out.clear();
    ts::Json j;
    if (!ts::Json::parse(json.data(), json.size(), j)) return false;
    walkSongs(j, out);
    return true;
}

Request visitorRequest()
{
    Request r;
    r.url = std::string(Gapis) + "visitor_id?prettyPrint=false";
    r.body = visionBody("", "");
    visionHeaders(r);
    return r;
}

std::string parseVisitor(const std::string& json)
{
    ts::Json j;
    if (!ts::Json::parse(json.data(), json.size(), j)) return "";
    return j["responseContext"]["visitorData"].s();
}

Request playerRequest(const std::string& videoId, const std::string& visitor)
{
    Request r;
    r.url = std::string(Gapis) + "player?prettyPrint=false&t=" + randomString(12) + "&id=" + videoId;
    r.body = visionBody(visitor, ",\"videoId\":" + ts::jsonQuote(videoId) + ",\"cpn\":\"" + randomString(16) +
                                 "\",\"contentCheckOk\":true,\"racyCheckOk\":true");
    visionHeaders(r);
    return r;
}

Audio parsePlayer(const std::string& json)
{
    Audio a;
    ts::Json j;
    if (!ts::Json::parse(json.data(), json.size(), j)) { a.error = "risposta non valida"; return a; }
    const ts::Json& st = j["playabilityStatus"];
    if (st["status"].s() != "OK") {
        a.error = "non disponibile: " + (st["reason"].s().empty() ? st["status"].s() : st["reason"].s());
        return a;
    }
    const ts::Json& f = j["streamingData"]["adaptiveFormats"];
    const ts::Json* best = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i]["itag"].i() == 140 && f[i].has("url")) {
            // with several audio tracks (dubbed videos) take the original
            if (!best || f[i]["audioTrack"]["audioIsDefault"].b) best = &f[i];
        }
    }
    if (!best) { a.error = "nessun audio AAC"; return a; }
    a.url = (*best)["url"].s();
    std::string len = (*best)["contentLength"].s();
    a.length = len.empty() ? -1 : std::atoll(len.c_str());
    return a;
}

Request audioRangeRequest(const Audio& a, long long from, long long to)
{
    Request r;
    char b[64];
    std::sprintf(b, "&range=%lld-%lld", from, to);
    r.url = a.url + b;
    r.headers.push_back(std::make_pair("User-Agent", std::string(VisionUa)));
    return r;
}

bool spotifyEmbedRequest(const std::string& link, Request& out)
{
    std::string kind, id;
    const char* kinds[] = { "playlist", "album", "track" };
    for (int k = 0; k < 3 && id.empty(); ++k) {
        std::string a = std::string("/") + kinds[k] + "/", b = std::string("spotify:") + kinds[k] + ":";
        size_t p = link.find(a);
        size_t start = p != std::string::npos ? p + a.size() : std::string::npos;
        if (start == std::string::npos) { p = link.find(b); if (p != std::string::npos) start = p + b.size(); }
        if (start == std::string::npos) continue;
        size_t e = start;
        while (e < link.size() && std::isalnum((unsigned char)link[e])) ++e;
        if (e - start >= 10) { kind = kinds[k]; id = link.substr(start, e - start); }
    }
    if (id.empty()) return false;
    out = Request();
    out.url = "https://open.spotify.com/embed/" + kind + "/" + id;
    out.headers.push_back(std::make_pair("User-Agent", std::string(BrowserUa)));
    return true;
}

// "Pitbull, Sensato": the embed page has a no-break space after the commas
static std::string spotifyArtists(const std::string& a)
{
    std::string clean;
    for (size_t k = 0; k < a.size(); ++k) {
        if ((unsigned char)a[k] == 0xC2 && k + 1 < a.size() && (unsigned char)a[k + 1] == 0xA0) { clean += ' '; ++k; }
        else clean += a[k];
    }
    return unescapeHtml(clean);
}

SpotifyList parseSpotifyEmbed(const std::string& html)
{
    SpotifyList l;
    const std::string tag = "<script id=\"__NEXT_DATA__\" type=\"application/json\">";
    size_t p = html.find(tag);
    size_t e = p == std::string::npos ? p : html.find("</script>", p);
    if (e == std::string::npos) { l.error = "pagina di Spotify non riconosciuta"; return l; }
    ts::Json j;
    std::string text = html.substr(p + tag.size(), e - p - tag.size());
    if (!ts::Json::parse(text.data(), text.size(), j)) { l.error = "dati di Spotify non leggibili"; return l; }
    const ts::Json& ent = j["props"]["pageProps"]["state"]["data"]["entity"];
    l.kind = ent["type"].s();
    l.name = ent["name"].s().empty() ? ent["title"].s() : ent["name"].s();
    const bool album = l.kind == "album";
    if (album) {
        l.artist = spotifyArtists(ent["subtitle"].s());
        const ts::Json& images = ent["visualIdentity"]["image"];
        for (size_t i = 0; i < images.size(); ++i)
            if (l.cover.empty() || images[i]["maxWidth"].num == 300) l.cover = images[i]["url"].s();
    }
    const ts::Json& tl = ent["trackList"];
    for (size_t i = 0; i < tl.size(); ++i) {
        SpotifyTrack t;
        t.title = unescapeHtml(tl[i]["title"].s());
        t.artist = spotifyArtists(tl[i]["subtitle"].s());
        t.seconds = (int)(tl[i]["duration"].num / 1000 + 0.5);
        if (album) { t.album = l.name; t.number = (int)i + 1; }
        if (!t.title.empty()) l.tracks.push_back(t);
    }
    if (l.tracks.empty() && ent["type"].s() == "track") {   // a single track page
        SpotifyTrack t;
        t.title = unescapeHtml(ent["name"].s());
        const ts::Json& artists = ent["artists"];
        for (size_t i = 0; i < artists.size(); ++i) t.artist += (i ? ", " : "") + artists[i]["name"].s();
        t.seconds = (int)(ent["duration"].num / 1000 + 0.5);
        if (!t.title.empty()) l.tracks.push_back(t);
    }
    if (l.tracks.empty()) l.error = "nessun brano (la playlist \xc3\xa8 privata?)";
    return l;
}

std::string queryFor(const SpotifyTrack& t)
{
    std::string first = t.artist.substr(0, t.artist.find(','));
    return t.title + " " + first;
}

int bestMatch(const SpotifyTrack& t, const std::vector<Song>& results)
{
    std::vector<std::string> want = words(t.title);
    std::string firstArtist = lower(t.artist.substr(0, t.artist.find(',')));
    int best = -1, bestScore = 0;
    for (size_t i = 0; i < results.size() && i < 8; ++i) {
        const Song& s = results[i];
        std::vector<std::string> have = words(s.title);
        int common = 0;
        for (size_t k = 0; k < want.size(); ++k)
            if (std::find(have.begin(), have.end(), want[k]) != have.end()) ++common;
        int score = want.empty() ? 0 : common * 100 / (int)want.size();
        if (!firstArtist.empty() && lower(s.artist).find(firstArtist) != std::string::npos) score += 50;
        if (t.seconds && s.seconds) {
            int d = std::abs(t.seconds - s.seconds);
            score += d <= 3 ? 30 : d <= 10 ? 15 : d > 40 ? -60 : 0;
        }
        if (!t.album.empty() && lower(s.album) == lower(t.album)) score += 20;   // the album's version, not a live one
        score -= (int)i * 2;   // the search order is a hint too
        if (score > bestScore) { bestScore = score; best = (int)i; }
    }
    return bestScore >= 80 ? best : -1;
}

namespace {
uint32_t be32(const std::string& d, size_t p)
{
    return ((uint32_t)(unsigned char)d[p] << 24) | ((uint32_t)(unsigned char)d[p + 1] << 16) |
           ((uint32_t)(unsigned char)d[p + 2] << 8) | (uint32_t)(unsigned char)d[p + 3];
}
void putBe32(std::string& d, size_t p, uint32_t v)
{
    d[p] = (char)(v >> 24); d[p + 1] = (char)(v >> 16); d[p + 2] = (char)(v >> 8); d[p + 3] = (char)v;
}
// [begin, end) of the child boxes of type t (32-bit sizes only, which is what YouTube writes)
bool findBox(const std::string& d, size_t begin, size_t end, const char* t, size_t& at, size_t& size)
{
    for (size_t p = begin; p + 8 <= end;) {
        uint32_t n = be32(d, p);
        if (n < 8 || p + n > end) return false;
        if (d.compare(p + 4, 4, t) == 0) { at = p; size = n; return true; }
        p += n;
    }
    return false;
}
}

int removeEditLists(std::string& d)
{
    size_t moov, moovSize;
    if (!findBox(d, 0, d.size(), "moov", moov, moovSize)) return 0;
    int removed = 0;
    size_t p = moov + 8;
    while (p + 8 <= moov + moovSize) {
        uint32_t n = be32(d, p);
        if (n < 8 || p + n > moov + moovSize) break;
        if (d.compare(p + 4, 4, "trak") == 0) {
            size_t edts, edtsSize;
            if (findBox(d, p + 8, p + n, "edts", edts, edtsSize)) {
                d.erase(edts, edtsSize);
                putBe32(d, p, n - (uint32_t)edtsSize);
                moovSize -= edtsSize;
                putBe32(d, moov, (uint32_t)moovSize);
                n -= (uint32_t)edtsSize;
                ++removed;
            }
        }
        p += n;
    }
    return removed;
}

namespace {
struct Box {
    std::string type;
    std::string data;        // payload of a leaf
    std::vector<Box> kids;   // children of a container
    bool container;
    Box() : container(false) {}
};

bool isContainer(const std::string& t)
{
    return t == "moov" || t == "trak" || t == "mdia" || t == "minf" || t == "stbl";
}

bool parseBoxes(const std::string& d, size_t p, size_t end, std::vector<Box>& out)
{
    while (p + 8 <= end) {
        uint32_t n = be32(d, p);
        if (n < 8 || p + n > end) return false;
        Box b;
        b.type = d.substr(p + 4, 4);
        if (isContainer(b.type)) {
            b.container = true;
            if (!parseBoxes(d, p + 8, p + n, b.kids)) return false;
        } else {
            b.data = d.substr(p + 8, n - 8);
        }
        out.push_back(b);
        p += n;
    }
    return p == end;
}

void writeBox(const Box& b, std::string& out)
{
    size_t at = out.size();
    out.append(4, '\0');
    out += b.type;
    if (b.container) for (size_t i = 0; i < b.kids.size(); ++i) writeBox(b.kids[i], out);
    else out += b.data;
    putBe32(out, at, (uint32_t)(out.size() - at));
}

Box* child(Box& b, const char* t)
{
    for (size_t i = 0; i < b.kids.size(); ++i) if (b.kids[i].type == t) return &b.kids[i];
    return 0;
}

void be32to(std::string& s, uint32_t v) { s.append(4, '\0'); putBe32(s, s.size() - 4, v); }

Box leaf(const char* t, const std::string& payload) { Box b; b.type = t; b.data = payload; return b; }

// the duration field of mvhd / tkhd / mdhd, version 0 or 1
void setDuration(Box& b, size_t v0Offset, size_t v1Offset, uint64_t value)
{
    if (b.data.empty()) return;
    if (b.data[0] == 0) { if (b.data.size() >= v0Offset + 4) putBe32(b.data, v0Offset, (uint32_t)value); }
    else if (b.data.size() >= v1Offset + 8) { putBe32(b.data, v1Offset, (uint32_t)(value >> 32)); putBe32(b.data, v1Offset + 4, (uint32_t)value); }
}
}

bool defragment(std::string& d, std::string& err)
{
    // top level
    std::vector<std::pair<std::string, std::pair<size_t, size_t> > > top;   // type, (start, size)
    for (size_t p = 0; p + 8 <= d.size();) {
        uint32_t n = be32(d, p);
        if (n < 8 || p + n > d.size()) { err = "box non valido"; return false; }
        top.push_back(std::make_pair(d.substr(p + 4, 4), std::make_pair(p, (size_t)n)));
        p += n;
    }
    size_t moovAt = 0, moovSize = 0;
    std::string ftyp;
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].first == "moov") { moovAt = top[i].second.first; moovSize = top[i].second.second; }
        if (top[i].first == "ftyp") ftyp = d.substr(top[i].second.first, top[i].second.second);
    }
    if (!moovSize) { err = "nessun moov"; return false; }
    std::vector<Box> mv;
    if (!parseBoxes(d, moovAt + 8, moovAt + moovSize, mv)) { err = "moov non valido"; return false; }
    Box moov;
    moov.type = "moov";
    moov.container = true;
    moov.kids = mv;
    // defaults of trex (mvex)
    uint32_t trexDuration = 0, trexSize = 0;
    for (size_t i = 0; i < moov.kids.size(); ++i) {
        if (moov.kids[i].type != "mvex") continue;
        const std::string& m = moov.kids[i].data;
        for (size_t p = 0; p + 8 <= m.size();) {
            uint32_t n = be32(m, p);
            if (n < 8) break;
            if (m.compare(p + 4, 4, "trex") == 0 && n >= 32) { trexDuration = be32(m, p + 20); trexSize = be32(m, p + 24); }
            p += n;
        }
    }
    int traks = 0;
    for (size_t i = 0; i < moov.kids.size(); ++i) if (moov.kids[i].type == "trak") ++traks;
    if (traks != 1) { err = "serve una sola traccia"; return false; }

    // samples of every fragment, in order: (offset in the file, size, duration); one chunk per trun
    std::vector<uint32_t> sizes, durations;
    std::vector<std::pair<size_t, uint32_t> > chunks;   // file offset, samples
    std::vector<std::pair<size_t, size_t> > ranges;    // data to copy
    for (size_t i = 0; i < top.size(); ++i) {
        if (top[i].first != "moof") continue;
        size_t moof = top[i].second.first, moofEnd = moof + top[i].second.second;
        for (size_t p = moof + 8; p + 8 <= moofEnd;) {
            uint32_t n = be32(d, p);
            if (n < 8 || p + n > moofEnd) { err = "moof non valido"; return false; }
            if (d.compare(p + 4, 4, "traf") == 0) {
                uint64_t base = moof;
                uint32_t defDuration = trexDuration, defSize = trexSize;
                for (size_t q = p + 8; q + 8 <= p + n;) {
                    uint32_t m = be32(d, q);
                    if (m < 8 || q + m > p + n) { err = "traf non valido"; return false; }
                    uint32_t flags = be32(d, q + 8) & 0xFFFFFF;
                    size_t f = q + 12;
                    if (d.compare(q + 4, 4, "tfhd") == 0) {
                        f += 4;   // track id
                        if (flags & 0x1) { base = ((uint64_t)be32(d, f) << 32) | be32(d, f + 4); f += 8; }
                        if (flags & 0x2) f += 4;
                        if (flags & 0x8) { defDuration = be32(d, f); f += 4; }
                        if (flags & 0x10) { defSize = be32(d, f); f += 4; }
                    } else if (d.compare(q + 4, 4, "trun") == 0) {
                        uint32_t count = be32(d, f); f += 4;
                        int32_t dataOffset = 0;
                        if (flags & 0x1) { dataOffset = (int32_t)be32(d, f); f += 4; }
                        if (flags & 0x4) f += 4;
                        size_t at = (size_t)(base + dataOffset), total = 0;
                        for (uint32_t k = 0; k < count; ++k) {
                            uint32_t dur = defDuration, size = defSize;
                            if (flags & 0x100) { dur = be32(d, f); f += 4; }
                            if (flags & 0x200) { size = be32(d, f); f += 4; }
                            if (flags & 0x400) f += 4;
                            if (flags & 0x800) f += 4;
                            if (f > q + m) { err = "trun troncato"; return false; }
                            sizes.push_back(size);
                            durations.push_back(dur);
                            total += size;
                        }
                        if (at + total > d.size()) { err = "dati fuori dal file"; return false; }
                        chunks.push_back(std::make_pair(at, count));
                        ranges.push_back(std::make_pair(at, total));
                    }
                    q += m;
                }
            }
            p += n;
        }
    }
    if (sizes.empty()) { err = "non frammentato"; return false; }

    uint64_t total = 0;
    for (size_t i = 0; i < durations.size(); ++i) total += durations[i];
    // sample tables
    std::string stts, stsz, stsc, stco;
    be32to(stts, 0);
    std::vector<std::pair<uint32_t, uint32_t> > runs;
    for (size_t i = 0; i < durations.size(); ++i) {
        if (!runs.empty() && runs.back().second == durations[i]) ++runs.back().first;
        else runs.push_back(std::make_pair(1u, durations[i]));
    }
    be32to(stts, (uint32_t)runs.size());
    for (size_t i = 0; i < runs.size(); ++i) { be32to(stts, runs[i].first); be32to(stts, runs[i].second); }
    be32to(stsz, 0); be32to(stsz, 0); be32to(stsz, (uint32_t)sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) be32to(stsz, sizes[i]);
    be32to(stsc, 0);
    std::string entries;
    uint32_t nEntries = 0, last = 0;
    for (size_t i = 0; i < chunks.size(); ++i) {
        if (chunks[i].second == last) continue;
        be32to(entries, (uint32_t)i + 1); be32to(entries, chunks[i].second); be32to(entries, 1);
        last = chunks[i].second;
        ++nEntries;
    }
    be32to(stsc, nEntries);
    stsc += entries;

    Box* trak = child(moov, "trak");
    Box* mdia = child(*trak, "mdia");
    Box* minf = mdia ? child(*mdia, "minf") : 0;
    Box* stbl = minf ? child(*minf, "stbl") : 0;
    if (!stbl) { err = "stbl mancante"; return false; }
    std::vector<Box> kept;
    for (size_t i = 0; i < stbl->kids.size(); ++i) {
        const std::string& t = stbl->kids[i].type;
        if (t == "stts" || t == "stsz" || t == "stz2" || t == "stsc" || t == "stco" || t == "co64") continue;
        kept.push_back(stbl->kids[i]);
    }
    kept.push_back(leaf("stts", stts));
    kept.push_back(leaf("stsc", stsc));
    kept.push_back(leaf("stsz", stsz));
    size_t stcoIndex = kept.size();
    std::string stcoEmpty;
    be32to(stcoEmpty, 0); be32to(stcoEmpty, (uint32_t)chunks.size());
    stcoEmpty.append(chunks.size() * 4, '\0');
    kept.push_back(leaf("stco", stcoEmpty));
    stbl->kids = kept;
    // no edit list, no mvex
    std::vector<Box> trakKids;
    for (size_t i = 0; i < trak->kids.size(); ++i) if (trak->kids[i].type != "edts") trakKids.push_back(trak->kids[i]);
    trak->kids = trakKids;
    std::vector<Box> moovKids;
    for (size_t i = 0; i < moov.kids.size(); ++i) if (moov.kids[i].type != "mvex") moovKids.push_back(moov.kids[i]);
    moov.kids = moovKids;
    trak = child(moov, "trak");
    mdia = child(*trak, "mdia");
    // durations: mdhd in the media timescale, mvhd and tkhd in the movie timescale
    Box* mdhd = child(*mdia, "mdhd");
    Box* mvhd = child(moov, "mvhd");
    Box* tkhd = child(*trak, "tkhd");
    uint32_t mediaScale = 0, movieScale = 0;
    if (mdhd && mdhd->data.size() >= 24) mediaScale = mdhd->data[0] == 0 ? be32(mdhd->data, 12) : be32(mdhd->data, 20);
    if (mvhd && mvhd->data.size() >= 24) movieScale = mvhd->data[0] == 0 ? be32(mvhd->data, 12) : be32(mvhd->data, 20);
    if (!mediaScale || !movieScale) { err = "timescale mancante"; return false; }
    uint64_t movieDuration = total * movieScale / mediaScale;
    setDuration(*mdhd, 16, 24, total);
    setDuration(*mvhd, 16, 24, movieDuration);
    setDuration(*tkhd, 20, 28, movieDuration);

    // a plain M4A brand; the moov first, then one mdat
    std::string out;
    std::string brand;
    brand += "M4A "; be32to(brand, 0); brand += "M4A mp42isom";
    Box ft = leaf("ftyp", brand);
    writeBox(ft, out);
    (void)ftyp;
    std::string moovBytes;
    writeBox(moov, moovBytes);
    size_t mdatStart = out.size() + moovBytes.size() + 8;
    // fill stco: find it again in the serialized moov (the last stco box: there is only one track)
    size_t stcoAt = moovBytes.rfind("stco");
    if (stcoAt == std::string::npos) { err = "stco"; return false; }
    size_t pos = mdatStart;
    for (size_t i = 0; i < chunks.size(); ++i) {
        putBe32(moovBytes, stcoAt + 4 + 8 + i * 4, (uint32_t)pos);
        pos += ranges[i].second;
    }
    (void)stcoIndex;
    out += moovBytes;
    size_t mdatSize = 8;
    for (size_t i = 0; i < ranges.size(); ++i) mdatSize += ranges[i].second;
    be32to(out, (uint32_t)mdatSize);
    out += "mdat";
    for (size_t i = 0; i < ranges.size(); ++i) out.append(d, ranges[i].first, ranges[i].second);
    d.swap(out);
    return true;
}

std::string formatSeconds(int s)
{
    char b[16];
    std::sprintf(b, "%d:%02d", s / 60, s % 60);
    return b;
}

std::string safeFileName(const std::string& s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
        r += c;
    }
    while (!r.empty() && (r[r.size() - 1] == '.' || r[r.size() - 1] == ' ')) r.erase(r.size() - 1);
    if (r.size() > 80) {
        size_t cut = 80;
        while (cut > 0 && ((unsigned char)r[cut] & 0xC0) == 0x80) --cut;   // not inside a UTF-8 character
        r = r.substr(0, cut);
    }
    return r.empty() ? "brano" : r;
}

}  // namespace online

#include "voice.h"

#include "opus.h"
#include "ogg/ogg.h"
#include "opusfile.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <vector>

namespace tg {

namespace {

void le16(std::string& o, int v) { o += static_cast<char>(v & 255); o += static_cast<char>((v >> 8) & 255); }
void le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o += static_cast<char>((v >> (8 * i)) & 255); }
uint32_t rd32(const std::string& s, std::size_t p) { return static_cast<unsigned char>(s[p]) | (static_cast<unsigned char>(s[p + 1]) << 8) |
    (static_cast<unsigned char>(s[p + 2]) << 16) | (static_cast<uint32_t>(static_cast<unsigned char>(s[p + 3])) << 24); }
int rd16(const std::string& s, std::size_t p) { return static_cast<unsigned char>(s[p]) | (static_cast<unsigned char>(s[p + 1]) << 8); }

std::string makeWav(const std::vector<int16_t>& pcm, int rate)
{
    std::string w;
    uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    w += "RIFF"; le32(w, 36 + data); w += "WAVE";
    w += "fmt "; le32(w, 16); le16(w, 1); le16(w, 1); le32(w, rate); le32(w, rate * 2); le16(w, 2); le16(w, 16);
    w += "data"; le32(w, data);
    if (!pcm.empty()) w.append(reinterpret_cast<const char*>(&pcm[0]), data);
    return w;
}

void flushPages(ogg_stream_state& os, std::string& out, bool force)
{
    ogg_page pg;
    while (force ? ogg_stream_flush(&os, &pg) : ogg_stream_pageout(&os, &pg)) {
        out.append(reinterpret_cast<const char*>(pg.header), pg.header_len);
        out.append(reinterpret_cast<const char*>(pg.body), pg.body_len);
    }
}

} // namespace

bool oggOpusToWav(const std::string& ogg, std::string& wav, int* seconds)
{
    int err = 0;
    OggOpusFile* f = op_open_memory(reinterpret_cast<const unsigned char*>(ogg.data()), ogg.size(), &err);
    if (!f) return false;
    std::vector<int16_t> out;
    out.reserve(ogg.size() * 8);
    opus_int16 buf[5760 * 2];
    int acc = 0, n3 = 0;
    for (;;) {
        int li = 0;
        int n = op_read(f, buf, sizeof(buf) / sizeof(buf[0]), &li);
        if (n < 0) { op_free(f); return false; }
        if (n == 0) break;
        int ch = op_channel_count(f, li);
        for (int i = 0; i < n; ++i) {
            int s = buf[i * ch];
            if (ch > 1) s = (s + buf[i * ch + 1]) / 2;
            acc += s;
            if (++n3 == 3) { out.push_back(static_cast<int16_t>(acc / 3)); acc = 0; n3 = 0; }   // 48 kHz -> 16 kHz
        }
    }
    op_free(f);
    if (seconds) *seconds = static_cast<int>((out.size() + 8000) / 16000);
    wav = makeWav(out, 16000);
    return true;
}

bool wavToOggOpus(const std::string& wav, std::string& ogg, int* seconds, std::string* waveform)
{
    if (wav.size() < 44 || wav.compare(0, 4, "RIFF") != 0 || wav.compare(8, 4, "WAVE") != 0) return false;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    std::size_t dataPos = 0, dataLen = 0, p = 12;
    while (p + 8 <= wav.size()) {
        std::string id = wav.substr(p, 4);
        uint32_t len = rd32(wav, p + 4);
        if (id == "fmt " && p + 24 <= wav.size()) {
            fmt = rd16(wav, p + 8); ch = rd16(wav, p + 10); rate = static_cast<int>(rd32(wav, p + 12)); bits = rd16(wav, p + 22);
        } else if (id == "data") {
            dataPos = p + 8;
            dataLen = std::min<std::size_t>(len, wav.size() - dataPos);
            break;
        }
        p += 8 + len + (len & 1);
    }
    if ((fmt != 1 && fmt != 0xFFFE) || ch < 1 || rate < 8000 || (bits != 16 && bits != 8) || !dataPos) return false;
    // mono samples at the source rate
    std::size_t frameBytes = static_cast<std::size_t>(ch) * (bits / 8);
    std::size_t frames = dataLen / frameBytes;
    std::vector<int> src(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        int s = 0;
        for (int c = 0; c < ch; ++c) {
            std::size_t q = dataPos + i * frameBytes + c * (bits / 8);
            s += bits == 16 ? static_cast<int16_t>(rd16(wav, q)) : (static_cast<unsigned char>(wav[q]) - 128) << 8;
        }
        src[i] = s / ch;
    }
    // linear resample to 16 kHz
    std::size_t n = static_cast<std::size_t>(static_cast<double>(frames) * 16000 / rate);
    std::vector<opus_int16> pcm(n);
    for (std::size_t i = 0; i < n; ++i) {
        double pos = static_cast<double>(i) * rate / 16000;
        std::size_t a = static_cast<std::size_t>(pos);
        double t = pos - a;
        int v0 = src[std::min(a, frames - 1)], v1 = src[std::min(a + 1, frames - 1)];
        pcm[i] = static_cast<opus_int16>(v0 + (v1 - v0) * t);
    }
    if (seconds) *seconds = static_cast<int>((n + 8000) / 16000);
    if (waveform) {
        const int kBars = 100;
        std::vector<int> peaks(kBars, 0);
        int maxPeak = 1;
        for (std::size_t i = 0; i < n; ++i) {
            int b = static_cast<int>(i * kBars / (n ? n : 1));
            int v = std::abs(static_cast<int>(pcm[i]));
            if (v > peaks[b]) peaks[b] = v;
        }
        for (int b = 0; b < kBars; ++b) if (peaks[b] > maxPeak) maxPeak = peaks[b];
        std::string w((kBars * 5 + 7) / 8, '\0');
        for (int b = 0; b < kBars; ++b) {
            int v = peaks[b] * 31 / maxPeak;
            int bit = b * 5;
            for (int k = 0; k < 5; ++k)
                if (v & (1 << k)) w[(bit + k) / 8] = static_cast<char>(w[(bit + k) / 8] | (1 << ((bit + k) % 8)));
        }
        *waveform = w;
    }

    int err = 0;
    OpusEncoder* enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (!enc) return false;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));

    ogg_stream_state os;
    ogg_stream_init(&os, static_cast<int>(rand()));
    ogg.clear();
    std::string head = "OpusHead";
    head += static_cast<char>(1);
    head += static_cast<char>(1);
    le16(head, lookahead * 3);
    le32(head, 16000);
    le16(head, 0);
    head += static_cast<char>(0);
    ogg_packet pk;
    std::memset(&pk, 0, sizeof(pk));
    pk.packet = reinterpret_cast<unsigned char*>(&head[0]);
    pk.bytes = static_cast<long>(head.size());
    pk.b_o_s = 1;
    ogg_stream_packetin(&os, &pk);
    flushPages(os, ogg, true);
    std::string tags = "OpusTags";
    const char* vendor = "dumbphone";
    le32(tags, static_cast<uint32_t>(std::strlen(vendor)));
    tags += vendor;
    le32(tags, 0);
    std::memset(&pk, 0, sizeof(pk));
    pk.packet = reinterpret_cast<unsigned char*>(&tags[0]);
    pk.bytes = static_cast<long>(tags.size());
    pk.packetno = 1;
    ogg_stream_packetin(&os, &pk);
    flushPages(os, ogg, true);

    const int kFrame = 320;   // 20 ms at 16 kHz
    unsigned char out[1500];
    std::size_t frameCount = (n + kFrame - 1) / kFrame;
    if (frameCount == 0) frameCount = 1;
    std::vector<opus_int16> frame(kFrame);
    for (std::size_t f = 0; f < frameCount; ++f) {
        for (int i = 0; i < kFrame; ++i) {
            std::size_t k = f * kFrame + i;
            frame[i] = k < n ? pcm[k] : 0;
        }
        int len = opus_encode(enc, &frame[0], kFrame, out, sizeof(out));
        if (len < 0) { opus_encoder_destroy(enc); ogg_stream_clear(&os); return false; }
        std::memset(&pk, 0, sizeof(pk));
        pk.packet = out;
        pk.bytes = len;
        pk.packetno = static_cast<ogg_int64_t>(2 + f);
        pk.granulepos = static_cast<ogg_int64_t>((f + 1) * kFrame * 3);
        pk.e_o_s = f + 1 == frameCount ? 1 : 0;
        ogg_stream_packetin(&os, &pk);
        flushPages(os, ogg, false);
    }
    flushPages(os, ogg, true);
    opus_encoder_destroy(enc);
    ogg_stream_clear(&os);
    return true;
}

} // namespace tg

// demuxtest <file> [seek-seconds] — prints what engine/port/media/Demux.cpp reads from a file, one line per sample
// ("<track> <pts us> <size> <K|->"), for engine/tools/demuxtest.py to compare with ffprobe's packets. PC only.
#include "../media/Demux.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Lumia::Media;

struct FileSource final : ByteSource {
    FILE* file { nullptr };
    long long total { 0 };
    long long size() final { return total; }
    bool read(unsigned long long offset, void* out, size_t size) final
    {
        if (offset + size > static_cast<unsigned long long>(total))
            return false;
        fseeko(file, static_cast<off_t>(offset), SEEK_SET);
        return fread(out, 1, size, file) == size;
    }
};

// Addresses are file paths: a reference is relative to the folder of its base.
struct FileFetcher final : Fetcher {
    bool fetch(const std::string& base, const std::string& reference, uint64_t offset, uint64_t length, std::vector<uint8_t>& out, std::string& resolved) final
    {
        resolved = base;
        if (!reference.empty()) {
            size_t slash = base.rfind('/');
            resolved = reference[0] == '/' || slash == std::string::npos ? reference : base.substr(0, slash + 1) + reference;
        }
        FILE* file = fopen(resolved.c_str(), "rb");
        if (!file)
            return false;
        fseeko(file, 0, SEEK_END);
        long long total = ftello(file);
        if (!length)
            length = static_cast<uint64_t>(total) - offset;
        out.resize(length);
        fseeko(file, static_cast<off_t>(offset), SEEK_SET);
        bool ok = fread(out.data(), 1, length, file) == length;
        fclose(file);
        return ok;
    }
    bool wait(int) final { return false; }
};

// demuxtest --mse <file> <piece size>: the file through the stream parser in pieces, same output.
struct PrintClient final : StreamParser::Client {
    void initialization(const std::vector<Track>& tracks, const std::vector<uint32_t>& ids, long long durationUs) final
    {
        printf("# mse, durata %lld us\n", durationUs);
        for (size_t i = 0; i < tracks.size(); ++i)
            printf("# traccia %zu: %s %s id %u %dx%d\n", i, tracks[i].video ? "video" : "audio", tracks[i].name.c_str(), ids[i], tracks[i].width, tracks[i].height);
    }
    void sample(size_t track, Sample&& sample) final
    {
        printf("%zu %lld %zu %c\n", track, sample.ptsUs, sample.data.size(), sample.key ? 'K' : '-');
    }
};

static int mse(const char* path, size_t piece)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return 2;
    auto parser = StreamParser::create();
    PrintClient client;
    std::vector<uint8_t> buffer(piece);
    size_t got;
    std::string error;
    while ((got = fread(buffer.data(), 1, piece, file)) > 0) {
        if (!parser->append(buffer.data(), got, client, error)) {
            printf("errore: %s\n", error.c_str());
            return 1;
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc >= 4 && std::string(argv[1]) == "--mse")
        return mse(argv[2], static_cast<size_t>(atoll(argv[3])));
    if (argc < 2)
        return 2;
    FileSource source;
    source.file = fopen(argv[1], "rb");
    if (!source.file)
        return 2;
    fseeko(source.file, 0, SEEK_END);
    source.total = ftello(source.file);
    std::string error;
    FileFetcher fetcher;
    auto demuxer = Demuxer::open(source, error, &fetcher, argv[1]);
    if (!demuxer) {
        printf("errore: %s\n", error.c_str());
        return 1;
    }
    printf("# %s, durata %lld us%s%s\n", demuxer->container(), demuxer->durationUs(), error.empty() ? "" : ", ", error.c_str());
    for (size_t i = 0; i < demuxer->tracks().size(); ++i) {
        auto& t = demuxer->tracks()[i];
        printf("# traccia %zu: %s %s %dx%d %d Hz %d canali, config %zu byte, durata %lld us\n", i, t.video ? "video" : "audio",
            t.name.c_str(), t.width, t.height, t.sampleRate, t.channels, t.config.size(), t.durationUs);
    }
    if (argc > 2)
        demuxer->seek(static_cast<long long>(atof(argv[2]) * 1000000));
    for (size_t i = 0; i < demuxer->tracks().size(); ++i) {
        Sample sample;
        while (demuxer->read(i, sample)) {
            size_t size = sample.data.size();
            printf("%zu %lld %zu %c\n", i, sample.ptsUs, size, sample.key ? 'K' : '-');
        }
        if (demuxer->failed())
            printf("# lettura fallita\n");
    }
    return 0;
}

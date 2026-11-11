// demuxtest <file> [seek-seconds] — prints what engine/port/media/Demux.cpp reads from a file, one line per sample
// ("<track> <pts us> <size> <K|->"), for engine/tools/demuxtest.py to compare with ffprobe's packets. PC only.
#include "../media/Demux.h"
#include <cstdio>
#include <cstdlib>

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

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    FileSource source;
    source.file = fopen(argv[1], "rb");
    if (!source.file)
        return 2;
    fseeko(source.file, 0, SEEK_END);
    source.total = ftello(source.file);
    std::string error;
    auto demuxer = Demuxer::open(source, error);
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

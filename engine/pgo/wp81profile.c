/* The part of the PGO runtime that is ours (PLAN.md §74): the instrumented engine (ENGINE_OUT=out/webkit-pgo, built
 * with -fprofile-generate) hands its counters to the port on command, which writes them in the app's data folder.
 * compiler-rt's own file layer (InstrProfilingFile.c: environment variables, memory-mapped merging, an atexit hook
 * that an app which never exits would not run) is left out; the writer below is its fileWriter over a callback.
 * Value profiles (indirect call targets, memcpy sizes) are included, which __llvm_profile_write_buffer leaves out. */
#include <winsock2.h>
#include "InstrProfiling.h"
#include "InstrProfilingInternal.h"

/* The instrumented objects pull this symbol in (/INCLUDE): upstream it drags the start-up hook of the file layer. */
COMPILER_RT_VISIBILITY int __llvm_profile_runtime = 0;

typedef int (*wp81ProfileSink)(void* context, const void* data, size_t size);
struct Sink {
    wp81ProfileSink write;
    void* context;
};

static uint32_t sinkWriter(ProfDataWriter* writer, ProfDataIOVec* vectors, uint32_t count)
{
    static const char zeroes[4096];
    struct Sink* sink = (struct Sink*)writer->WriterCtx;
    for (uint32_t i = 0; i < count; ++i) {
        size_t size = vectors[i].ElmSize * vectors[i].NumElm;
        if (vectors[i].Data) {
            if (sink->write(sink->context, vectors[i].Data, size))
                return 1;
            continue;
        }
        /* Padding, or a skip in a file that is new: zeroes either way. */
        while (size) {
            size_t part = size < sizeof(zeroes) ? size : sizeof(zeroes);
            if (sink->write(sink->context, zeroes, part))
                return 1;
            size -= part;
        }
    }
    return 0;
}

/* The whole raw profile (.profraw) through the sink, in order; 0 when everything was written. */
int wp81ProfileWrite(wp81ProfileSink write, void* context)
{
    struct Sink sink = { write, context };
    ProfDataWriter writer = { sinkWriter, &sink };
    return lprofWriteData(&writer, lprofGetVPDataReader(), 0);
}

/* Counters back to zero: what ran before (the app's start) leaves the profile. */
void wp81ProfileReset(void)
{
    __llvm_profile_reset_counters();
}

/* The profile file goes to the PC as the logs do, a POST to tools/inbox (192.168.1.148:8765, /profraw, chunked): the
 * app's folder cannot be read over USB while the app is open, and nothing closes the app from the PC.
 * Begin, any number of Data, End; each returns 0 when it worked. */
static SOCKET uploadSocket = INVALID_SOCKET;

static int sendAll(const char* data, size_t size)
{
    while (size) {
        int sent = send(uploadSocket, data, size > 65536 ? 65536 : (int)size, 0);
        if (sent <= 0)
            return 1;
        data += sent;
        size -= sent;
    }
    return 0;
}

int wp81ProfileUploadBegin(void)
{
    static const char header[] = "POST /profraw HTTP/1.1\r\nHost: pc\r\nTransfer-Encoding: chunked\r\n\r\n";
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_port = htons(8765);
    address.sin_addr.s_addr = htonl((192u << 24) | (168u << 16) | (1u << 8) | 148u);
    uploadSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (uploadSocket == INVALID_SOCKET)
        return 1;
    if (connect(uploadSocket, (struct sockaddr*)&address, sizeof address))
        return 1;
    return sendAll(header, sizeof header - 1);
}

int wp81ProfileUploadData(const void* data, size_t size)
{
    char line[16];
    int length = 0;
    size_t left = size;
    char digits[9];
    int count = 0;
    if (!size)
        return 0;
    do {
        digits[count++] = "0123456789abcdef"[left & 15];
        left >>= 4;
    } while (left);
    while (count)
        line[length++] = digits[--count];
    line[length++] = '\r';
    line[length++] = '\n';
    return sendAll(line, length) || sendAll((const char*)data, size) || sendAll("\r\n", 2);
}

int wp81ProfileUploadEnd(void)
{
    char reply[256];
    int failed = sendAll("0\r\n\r\n", 5);
    /* The PC answers when it has the whole body. */
    if (!failed)
        failed = recv(uploadSocket, reply, sizeof reply, 0) <= 0;
    closesocket(uploadSocket);
    uploadSocket = INVALID_SOCKET;
    return failed;
}

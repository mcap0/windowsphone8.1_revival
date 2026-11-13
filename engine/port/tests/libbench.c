/*
 * libbench — the engine's decoding libraries off the phone (engine/tools/libbench.py, PLAN.md section 77): built for
 * Linux ARMv7 with the phone's compiler flags and run under a counting qemu-arm (engine/tools/armcount), so two builds
 * of zlib or libpng compare by the instructions they execute.
 *
 *   libbench inflate FILE... : each file gzip-compressed once (level 6, as a server sends it), then inflated 5 times
 *                              (LIBBENCH_ROUNDS: another count; 0 measures the compression alone)
 *   libbench png FILE...     : each PNG decoded 5 times to RGBA through libpng's progressive reader, as WebKit's
 *                              PNGImageDecoder reads it (png_process_data, a row callback)
 * Prints a checksum of what came out, so two builds can be checked to agree.
 */
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static unsigned char* readFile(const char* path, size_t* size)
{
    FILE* file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char* data = malloc(length ? length : 1);
    *size = fread(data, 1, length, file);
    fclose(file);
    return data;
}

static uint32_t checksum(uint32_t sum, const unsigned char* data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        sum = sum * 31 + data[i];
    return sum;
}

static int inflateBench(const char* path, uint32_t* sum)
{
    size_t size;
    unsigned char* data = readFile(path, &size);
    if (!data)
        return 1;
    z_stream stream = { 0 };
    deflateInit2(&stream, 6, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY);
    uLong bound = deflateBound(&stream, size) + 64;
    unsigned char* compressed = malloc(bound);
    stream.next_in = data;
    stream.avail_in = size;
    stream.next_out = compressed;
    stream.avail_out = bound;
    deflate(&stream, Z_FINISH);
    size_t compressedSize = stream.total_out;
    deflateEnd(&stream);
    unsigned char* out = malloc(size + 1);
    const char* roundsText = getenv("LIBBENCH_ROUNDS");
    int rounds = roundsText ? atoi(roundsText) : 5;
    memcpy(out, data, size);
    for (int round = 0; round < rounds; ++round) {
        z_stream in = { 0 };
        inflateInit2(&in, 31);
        // As curl feeds a body: 16 KB at a time.
        size_t offset = 0;
        in.next_out = out;
        in.avail_out = size + 1;
        int status = Z_OK;
        while (status == Z_OK && offset < compressedSize) {
            size_t chunk = compressedSize - offset < 16384 ? compressedSize - offset : 16384;
            in.next_in = compressed + offset;
            in.avail_in = chunk;
            status = inflate(&in, Z_NO_FLUSH);
            offset += chunk - in.avail_in;
        }
        if (in.total_out != size || memcmp(out, data, size)) {
            fprintf(stderr, "%s: inflate gave %lu bytes, not %zu\n", path, in.total_out, size);
            return 1;
        }
        inflateEnd(&in);
    }
    *sum = checksum(*sum, out, size);
    printf("%s: %zu -> %zu bytes\n", path, size, compressedSize);
    free(out);
    free(compressed);
    free(data);
    return 0;
}

struct Decode {
    png_structp png;
    png_infop info;
    unsigned char* pixels;
    png_uint_32 width;
    png_uint_32 height;
    int passes;
};

static void infoCallback(png_structp png, png_infop info)
{
    struct Decode* decode = png_get_progressive_ptr(png);
    int bitDepth, colorType, interlace;
    png_get_IHDR(png, info, &decode->width, &decode->height, &bitDepth, &colorType, &interlace, NULL, NULL);
    // WebKit's transformations (PNGImageDecoder::headerAvailable): everything to 8-bit RGBA.
    if (colorType == PNG_COLOR_TYPE_PALETTE || (colorType == PNG_COLOR_TYPE_GRAY && bitDepth < 8))
        png_set_expand(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_expand(png);
    if (bitDepth == 16)
        png_set_strip_16(png);
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    decode->passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    decode->pixels = calloc((size_t)decode->width * decode->height, 4);
}

static void rowCallback(png_structp png, png_bytep row, png_uint_32 index, int pass)
{
    struct Decode* decode = png_get_progressive_ptr(png);
    if (!row || index >= decode->height)
        return;
    png_progressive_combine_row(png, decode->pixels + (size_t)index * decode->width * 4, row);
    (void)pass;
}

static int pngBench(const char* path, uint32_t* sum)
{
    size_t size;
    unsigned char* data = readFile(path, &size);
    if (!data)
        return 1;
    for (int round = 0; round < 5; ++round) {
        struct Decode decode = { 0 };
        decode.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        decode.info = png_create_info_struct(decode.png);
        if (setjmp(png_jmpbuf(decode.png))) {
            fprintf(stderr, "%s: libpng error\n", path);
            return 1;
        }
        png_set_progressive_read_fn(decode.png, &decode, infoCallback, rowCallback, NULL);
        // As the network delivers it: 8 KB at a time.
        for (size_t offset = 0; offset < size; offset += 8192)
            png_process_data(decode.png, decode.info, data + offset, size - offset < 8192 ? size - offset : 8192);
        if (round == 4 && decode.pixels) {
            *sum = checksum(*sum, decode.pixels, (size_t)decode.width * decode.height * 4);
            printf("%s: %ux%u\n", path, decode.width, decode.height);
        }
        free(decode.pixels);
        png_destroy_read_struct(&decode.png, &decode.info, NULL);
    }
    free(data);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3)
        return 2;
    uint32_t sum = 0;
    for (int i = 2; i < argc; ++i) {
        int failed = !strcmp(argv[1], "inflate") ? inflateBench(argv[i], &sum) : pngBench(argv[i], &sum);
        if (failed)
            return 1;
    }
    printf("checksum %08x\n", sum);
    return 0;
}

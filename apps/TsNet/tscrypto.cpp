#include "tscrypto.h"

#include <cstring>

#include "tsmbed.h"

#include <cstdio>
#include <cstdlib>

namespace ts {

static RandomFn gRandom = nullptr;
void setRandomSource(RandomFn fn) { gRandom = fn; }

void randomBytes(uint8_t* out, size_t n) {
#ifdef _WIN32
    if (!gRandom) abort();   // the app installs CryptographicBuffer::GenerateRandom at startup
    gRandom(out, n);
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(out, 1, n, f) != n) abort();
    fclose(f);
#endif
}

// ---- BLAKE2s (RFC 7693)

static const uint32_t kIV[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};
static const uint8_t kSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0}};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void compress(Blake2s& s, const uint8_t* block, bool last) {
    uint32_t m[16], v[16];
    for (int i = 0; i < 16; i++) m[i] = le32(block + 4 * i);
    for (int i = 0; i < 8; i++) { v[i] = s.h[i]; v[i + 8] = kIV[i]; }
    v[12] ^= s.t[0];
    v[13] ^= s.t[1];
    if (last) v[14] = ~v[14];
#define G(a, b, c, d, x, y)            \
    v[a] = v[a] + v[b] + x; v[d] = rotr(v[d] ^ v[a], 16); \
    v[c] = v[c] + v[d]; v[b] = rotr(v[b] ^ v[c], 12);     \
    v[a] = v[a] + v[b] + y; v[d] = rotr(v[d] ^ v[a], 8);  \
    v[c] = v[c] + v[d]; v[b] = rotr(v[b] ^ v[c], 7);
    for (int r = 0; r < 10; r++) {
        const uint8_t* z = kSigma[r];
        G(0, 4, 8, 12, m[z[0]], m[z[1]]) G(1, 5, 9, 13, m[z[2]], m[z[3]])
        G(2, 6, 10, 14, m[z[4]], m[z[5]]) G(3, 7, 11, 15, m[z[6]], m[z[7]])
        G(0, 5, 10, 15, m[z[8]], m[z[9]]) G(1, 6, 11, 12, m[z[10]], m[z[11]])
        G(2, 7, 8, 13, m[z[12]], m[z[13]]) G(3, 4, 9, 14, m[z[14]], m[z[15]])
    }
#undef G
    for (int i = 0; i < 8; i++) s.h[i] ^= v[i] ^ v[i + 8];
}

Blake2s::Blake2s(size_t outLen_, const uint8_t* key, size_t keyLen) : len(0), outLen(outLen_) {
    memcpy(h, kIV, sizeof h);
    h[0] ^= 0x01010000 ^ ((uint32_t)keyLen << 8) ^ (uint32_t)outLen;
    t[0] = t[1] = 0;
    if (keyLen) {
        uint8_t block[64] = {0};
        memcpy(block, key, keyLen);
        update(block, 64);
    }
}

void Blake2s::update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    while (n > 0) {
        if (len == 64) {   // only compress a full buffer once more data follows (the last block is special)
            t[0] += 64;
            if (t[0] < 64) t[1]++;
            compress(*this, buf, false);
            len = 0;
        }
        size_t k = 64 - len < n ? 64 - len : n;
        memcpy(buf + len, p, k);
        len += k; p += k; n -= k;
    }
}

void Blake2s::final(uint8_t* out) {
    t[0] += (uint32_t)len;
    if (t[0] < len) t[1]++;
    memset(buf + len, 0, 64 - len);
    compress(*this, buf, true);
    uint8_t full[32];
    for (int i = 0; i < 8; i++) put32(full + 4 * i, h[i]);
    memcpy(out, full, outLen);
}

void blake2s(uint8_t out[32], const void* a, size_t an, const void* b, size_t bn) {
    Blake2s s;
    s.update(a, an);
    if (bn) s.update(b, bn);
    s.final(out);
}

void hmacBlake2s(uint8_t out[32], const uint8_t* key, size_t keyLen, const void* data, size_t n) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    if (keyLen > 64) blake2s(k, key, keyLen);
    else memcpy(k, key, keyLen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    Blake2s a;
    a.update(pad, 64);
    a.update(data, n);
    a.final(inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    Blake2s b;
    b.update(pad, 64);
    b.update(inner, 32);
    b.final(out);
}

void kdf(const uint8_t ck[32], const uint8_t* input, size_t n, uint8_t* o1, uint8_t* o2, uint8_t* o3) {
    uint8_t prk[32], t[33], r[32];
    hmacBlake2s(prk, ck, 32, input, n);
    t[0] = 1;
    hmacBlake2s(r, prk, 32, t, 1);
    memcpy(o1, r, 32);
    if (!o2) return;
    memcpy(t, r, 32);
    t[32] = 2;
    hmacBlake2s(r, prk, 32, t, 33);
    memcpy(o2, r, 32);
    if (!o3) return;
    memcpy(t, r, 32);
    t[32] = 3;
    hmacBlake2s(r, prk, 32, t, 33);
    memcpy(o3, r, 32);
}

// ---- X25519 through mbedTLS' Montgomery ladder

static bool scalarMult(uint8_t out[32], const uint8_t priv[32], const uint8_t* pub) {
    if (tsm_x25519(out, priv, pub, randomBytes) != 0) return false;
    uint8_t z = 0;
    for (int i = 0; i < 32; i++) z |= out[i];
    return z != 0;
}

void x25519Base(uint8_t pub[32], const uint8_t priv[32]) { scalarMult(pub, priv, nullptr); }
bool x25519(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32]) { return scalarMult(out, priv, pub); }

void newKeyPair(uint8_t priv[32], uint8_t pub[32]) {
    randomBytes(priv, 32);
    priv[0] &= 248; priv[31] &= 127; priv[31] |= 64;
    x25519Base(pub, priv);
}

// ---- ChaCha20-Poly1305

void aeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
              const uint8_t* ad, size_t adn, uint8_t* out) {
    tsm_aead_seal(key, nonce, in, n, ad, adn, out);
}

bool aeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
              const uint8_t* ad, size_t adn, uint8_t* out) {
    return tsm_aead_open(key, nonce, in, n, ad, adn, out) == 0;
}

// ---- NaCl box: HSalsa20 key derivation, XSalsa20 stream, Poly1305

static void salsaCore(uint32_t x[16], int rounds) {
#define QR(a, b, c, d)  \
    x[b] ^= rotl(x[a] + x[d], 7); x[c] ^= rotl(x[b] + x[a], 9); \
    x[d] ^= rotl(x[c] + x[b], 13); x[a] ^= rotl(x[d] + x[c], 18);
    for (int i = 0; i < rounds; i += 2) {
        QR(0, 4, 8, 12) QR(5, 9, 13, 1) QR(10, 14, 2, 6) QR(15, 3, 7, 11)
        QR(0, 1, 2, 3) QR(5, 6, 7, 4) QR(10, 11, 8, 9) QR(15, 12, 13, 14)
    }
#undef QR
}

static void salsaInit(uint32_t s[16], const uint8_t key[32], const uint8_t in[16]) {
    s[0] = 0x61707865; s[5] = 0x3320646e; s[10] = 0x79622d32; s[15] = 0x6b206574;
    for (int i = 0; i < 4; i++) {
        s[1 + i] = le32(key + 4 * i);
        s[11 + i] = le32(key + 16 + 4 * i);
        s[6 + i] = le32(in + 4 * i);
    }
}

static void hsalsa20(uint8_t out[32], const uint8_t key[32], const uint8_t in[16]) {
    uint32_t x[16];
    salsaInit(x, key, in);
    salsaCore(x, 20);
    static const int idx[8] = {0, 5, 10, 15, 6, 7, 8, 9};
    for (int i = 0; i < 8; i++) put32(out + 4 * i, x[idx[i]]);
}

// XOR with the XSalsa20 keystream starting at block 0 (the first 32 bytes are kept for the Poly1305 key)
static void xsalsa20(uint8_t* data, size_t n, const uint8_t key[32], const uint8_t nonce[24]) {
    uint8_t sub[32], in[16];
    hsalsa20(sub, key, nonce);
    memcpy(in, nonce + 16, 8);
    uint64_t ctr = 0;
    for (size_t off = 0; off < n; off += 64, ctr++) {
        for (int i = 0; i < 8; i++) in[8 + i] = (uint8_t)(ctr >> (8 * i));
        uint32_t s[16], x[16];
        salsaInit(s, sub, in);
        memcpy(x, s, sizeof x);
        salsaCore(x, 20);
        uint8_t ks[64];
        for (int i = 0; i < 16; i++) put32(ks + 4 * i, x[i] + s[i]);
        for (size_t i = 0; i < 64 && off + i < n; i++) data[off + i] ^= ks[i];
    }
}

static bool boxKey(uint8_t k[32], const uint8_t peerPub[32], const uint8_t priv[32]) {
    uint8_t shared[32], zero[16] = {0};
    if (!x25519(shared, priv, peerPub)) return false;
    hsalsa20(k, shared, zero);
    return true;
}

Bytes boxSeal(const uint8_t peerPub[32], const uint8_t priv[32], const uint8_t* msg, size_t n) {
    uint8_t k[32];
    Bytes out(24 + 16 + n);
    randomBytes(&out[0], 24);
    if (!boxKey(k, peerPub, priv)) return Bytes();
    Bytes buf(32 + n, 0);   // 32 zero bytes give the Poly1305 key, then the message
    if (n) memcpy(&buf[32], msg, n);
    xsalsa20(&buf[0], buf.size(), k, &out[0]);
    tsm_poly1305(&buf[0], &buf[32], n, &out[24]);
    if (n) memcpy(&out[40], &buf[32], n);
    return out;
}

bool boxOpen(const uint8_t peerPub[32], const uint8_t priv[32], const uint8_t* in, size_t n, Bytes& out) {
    if (n < 40) return false;
    uint8_t k[32], tag[16];
    if (!boxKey(k, peerPub, priv)) return false;
    size_t m = n - 40;
    Bytes buf(32 + m, 0);
    if (m) memcpy(&buf[32], in + 40, m);
    uint8_t polyKey[32] = {0};
    xsalsa20(polyKey, 32, k, in);
    tsm_poly1305(polyKey, in + 40, m, tag);
    uint8_t d = 0;
    for (int i = 0; i < 16; i++) d |= tag[i] ^ in[24 + i];
    if (d) return false;
    xsalsa20(&buf[0], buf.size(), k, in);
    out.assign(buf.begin() + 32, buf.end());
    return true;
}

// ---- encodings

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

static int nib(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool unhex(const std::string& s, uint8_t* out, size_t n) {
    if (s.size() != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        int a = nib(s[2 * i]), b = nib(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return true;
}

std::string base64(const uint8_t* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        s += t[v >> 18 & 63];
        s += t[v >> 12 & 63];
        s += i + 1 < n ? t[v >> 6 & 63] : '=';
        s += i + 2 < n ? t[v & 63] : '=';
    }
    return s;
}

}  // namespace ts

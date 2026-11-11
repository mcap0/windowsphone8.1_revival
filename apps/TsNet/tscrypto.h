// Crypto for the mini Tailscale client: BLAKE2s (+HMAC, Noise HKDF), X25519 and ChaCha20-Poly1305 (mbedTLS),
// NaCl box (Curve25519 + XSalsa20-Poly1305, used by DERP). Portable C++11: the same code is tested on the PC.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf   // VS2013 (v120_wp81)
#endif

namespace ts {

typedef std::vector<uint8_t> Bytes;

typedef void (*RandomFn)(uint8_t* out, size_t n);
void setRandomSource(RandomFn fn);   // required on the phone (WinRT CSPRNG); the PC uses /dev/urandom
void randomBytes(uint8_t* out, size_t n);

struct Blake2s {
    uint32_t h[8], t[2];
    uint8_t buf[64];
    size_t len, outLen;
    Blake2s(size_t outLen = 32, const uint8_t* key = nullptr, size_t keyLen = 0);
    void update(const void* data, size_t n);
    void final(uint8_t* out);
};
void blake2s(uint8_t out[32], const void* a, size_t an, const void* b = nullptr, size_t bn = 0);
void hmacBlake2s(uint8_t out[32], const uint8_t* key, size_t keyLen, const void* data, size_t n);
// Noise/WireGuard KDF: up to three 32-byte outputs from chaining key ck and input
void kdf(const uint8_t ck[32], const uint8_t* input, size_t n, uint8_t* o1, uint8_t* o2 = nullptr, uint8_t* o3 = nullptr);

void x25519Base(uint8_t pub[32], const uint8_t priv[32]);
bool x25519(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32]);
void newKeyPair(uint8_t priv[32], uint8_t pub[32]);

// ChaCha20-Poly1305 with a full 12-byte nonce; out gets n + 16 bytes (ciphertext || tag)
void aeadSeal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
              const uint8_t* ad, size_t adn, uint8_t* out);
bool aeadOpen(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
              const uint8_t* ad, size_t adn, uint8_t* out);

// NaCl box as Go's x/crypto/nacl/box: Seal output = nonce(24) || tag(16) || ciphertext
Bytes boxSeal(const uint8_t peerPub[32], const uint8_t priv[32], const uint8_t* msg, size_t n);
bool boxOpen(const uint8_t peerPub[32], const uint8_t priv[32], const uint8_t* in, size_t n, Bytes& out);

std::string hex(const uint8_t* p, size_t n);
bool unhex(const std::string& s, uint8_t* out, size_t n);
std::string base64(const uint8_t* p, size_t n);

}  // namespace ts

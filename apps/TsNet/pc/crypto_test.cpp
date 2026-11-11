// Prints crypto outputs for fixed inputs; crypto_test.py checks them against Python (hashlib, cryptography, PyNaCl).
#include "../tscrypto.h"
#include <cstdio>
#include <cstring>
using namespace ts;
int main(int argc, char** argv) {
    uint8_t data[300];
    for (int i = 0; i < 300; i++) data[i] = (uint8_t)(i * 7 + 1);
    int lens[] = {0, 1, 63, 64, 65, 128, 300};
    for (int l : lens) { uint8_t o[32]; blake2s(o, data, l); printf("b2s %d %s\n", l, hex(o, 32).c_str()); }
    { Blake2s s(16, data, 32); s.update(data, 116); uint8_t o[16]; s.final(o); printf("b2k %s\n", hex(o, 16).c_str()); }
    { uint8_t o[32]; hmacBlake2s(o, data, 32, data + 40, 100); printf("hmac %s\n", hex(o, 32).c_str()); }
    { uint8_t a[32], b[32], c[32]; kdf(data, data + 50, 32, a, b, c); printf("kdf %s %s %s\n", hex(a,32).c_str(), hex(b,32).c_str(), hex(c,32).c_str()); }
    { uint8_t pub[32], sh[32]; x25519Base(pub, data); printf("xpub %s\n", hex(pub, 32).c_str());
      x25519(sh, data, data + 100); printf("xsh %s\n", hex(sh, 32).c_str()); }
    { uint8_t out[116]; aeadSeal(data, data + 32, data + 50, 100, data + 200, 13, out); printf("aead %s\n", hex(out, 116).c_str());
      uint8_t back[100]; int ok = aeadOpen(data, data + 32, out, 116, data + 200, 13, back); printf("aeadopen %d %d\n", ok, memcmp(back, data + 50, 100) == 0); }
    { uint8_t pub2[32]; x25519Base(pub2, data + 64);   // box from key data[0:32] to key data[64:96]
      Bytes b = boxSeal(pub2, data, data + 10, 77); printf("box %s\n", hex(&b[0], b.size()).c_str());
      if (argc > 1) { uint8_t pub1[32]; x25519Base(pub1, data); std::string h = argv[1]; Bytes in(h.size() / 2); unhex(h, &in[0], in.size());
        Bytes m; bool ok = boxOpen(pub1, data + 64, &in[0], in.size(), m); printf("boxopen %d %s\n", ok, hex(m.data(), m.size()).c_str()); } }
}

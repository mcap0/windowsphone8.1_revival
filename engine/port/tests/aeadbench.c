/*
 * aeadbench — LibreSSL's two TLS record ciphers as the phone has them (C only: no assembly for Windows on ARM32), off
 * the phone (PLAN.md section 77): 4 MB decrypted in 16 KB records (a TLS record's size) with the cipher named, run
 * under engine/tools/armcount so the two compare by instructions. Krait has no AES instructions.
 *   aeadbench aes-128-gcm | aes-256-gcm | chacha20-poly1305 | none
 */
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    const EVP_AEAD* aead = NULL;
    if (argc < 2)
        return 2;
    if (!strcmp(argv[1], "aes-128-gcm"))
        aead = EVP_aead_aes_128_gcm();
    else if (!strcmp(argv[1], "aes-256-gcm"))
        aead = EVP_aead_aes_256_gcm();
    else if (!strcmp(argv[1], "chacha20-poly1305"))
        aead = EVP_aead_chacha20_poly1305();
    static unsigned char key[32], nonce[12], record[16384 + 16], plain[16384 + 16];
    if (!aead)
        return 0;
    EVP_AEAD_CTX* context = EVP_AEAD_CTX_new();
    EVP_AEAD_CTX_init(context, aead, key, EVP_AEAD_key_length(aead), EVP_AEAD_DEFAULT_TAG_LENGTH, NULL);
    size_t sealed = 0;
    EVP_AEAD_CTX_seal(context, record, &sealed, sizeof(record), nonce, sizeof(nonce), plain, 16384, NULL, 0);
    unsigned long total = 0;
    for (int i = 0; i < 256; ++i) {
        size_t opened = 0;
        if (!EVP_AEAD_CTX_open(context, plain, &opened, sizeof(plain), nonce, sizeof(nonce), record, sealed, NULL, 0))
            return 1;
        total += opened;
    }
    printf("%s: %lu bytes\n", argv[1], total);
    return 0;
}

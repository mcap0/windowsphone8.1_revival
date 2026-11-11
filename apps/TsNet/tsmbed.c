#include "tsmbed.h"

#include <stdlib.h>
#include <string.h>

#include "mbedtls/chachapoly.h"
#include "mbedtls/ecp.h"
#include "mbedtls/poly1305.h"
#include "mbedtls/ssl.h"

static int rng_adapter(void* f, unsigned char* out, size_t n) {
    ((tsm_rng)f)(out, n);
    return 0;
}

int tsm_x25519(uint8_t out[32], const uint8_t priv[32], const uint8_t* pub, tsm_rng rng) {
    mbedtls_ecp_group g;
    mbedtls_mpi d;
    mbedtls_ecp_point P, R;
    uint8_t k[32];
    size_t olen = 0;
    int ok;
    mbedtls_ecp_group_init(&g);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&P);
    mbedtls_ecp_point_init(&R);
    memcpy(k, priv, 32);
    k[0] &= 248; k[31] &= 127; k[31] |= 64;
    ok = mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
         mbedtls_mpi_read_binary_le(&d, k, 32) == 0 &&
         (pub ? mbedtls_ecp_point_read_binary(&g, &P, pub, 32) : mbedtls_ecp_copy(&P, &g.G)) == 0 &&
         mbedtls_ecp_mul(&g, &R, &d, &P, rng_adapter, (void*)rng) == 0 &&
         mbedtls_ecp_point_write_binary(&g, &R, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out, 32) == 0 && olen == 32;
    mbedtls_ecp_point_free(&R);
    mbedtls_ecp_point_free(&P);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&g);
    memset(k, 0, sizeof k);
    return ok ? 0 : -1;
}

void tsm_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
                   const uint8_t* ad, size_t adn, uint8_t* out) {
    mbedtls_chachapoly_context c;
    mbedtls_chachapoly_init(&c);
    mbedtls_chachapoly_setkey(&c, key);
    mbedtls_chachapoly_encrypt_and_tag(&c, n, nonce, ad, adn, in, out, out + n);
    mbedtls_chachapoly_free(&c);
}

int tsm_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
                  const uint8_t* ad, size_t adn, uint8_t* out) {
    mbedtls_chachapoly_context c;
    int r;
    if (n < 16) return -1;
    mbedtls_chachapoly_init(&c);
    mbedtls_chachapoly_setkey(&c, key);
    r = mbedtls_chachapoly_auth_decrypt(&c, n - 16, nonce, ad, adn, in + n - 16, in, out);
    mbedtls_chachapoly_free(&c);
    return r;
}

void tsm_poly1305(const uint8_t key[32], const uint8_t* in, size_t n, uint8_t tag[16]) {
    mbedtls_poly1305_mac(key, in, n, tag);
}

struct tsm_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
};

tsm_tls* tsm_tls_new(const char* host, void* ctx, tsm_send send, tsm_recv recv, tsm_rng rng) {
    tsm_tls* t = (tsm_tls*)calloc(1, sizeof *t);
    if (!t) return NULL;
    mbedtls_ssl_init(&t->ssl);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_max_tls_version(&t->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&t->conf, rng_adapter, (void*)rng);
    if (mbedtls_ssl_setup(&t->ssl, &t->conf) != 0) {
        tsm_tls_free(t);
        return NULL;
    }
    mbedtls_ssl_set_hostname(&t->ssl, host);
    mbedtls_ssl_set_bio(&t->ssl, ctx, send, recv, NULL);
    return t;
}

#ifdef _MSC_VER
/* A fault inside mbedTLS (seen on the phone with RSA servers: the Samsung TV, open.spotify.com) becomes an error
   with its code and address instead of killing the app: tsm_last_fault tells what and where. */
#include <windows.h>
static volatile unsigned long tsm_fault_code;
static void* volatile tsm_fault_addr;
static int tsm_filter(EXCEPTION_POINTERS* e)
{
    tsm_fault_code = e->ExceptionRecord->ExceptionCode;
    tsm_fault_addr = e->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
static int handshake_raw(tsm_tls* t) { return mbedtls_ssl_handshake(&t->ssl); }
static int read_raw(tsm_tls* t, uint8_t* p, size_t n) { return mbedtls_ssl_read(&t->ssl, p, n); }
int tsm_tls_handshake(tsm_tls* t)
{
    __try { return handshake_raw(t); }
    __except (tsm_filter(GetExceptionInformation())) { return TSM_FAULT; }
}
int tsm_tls_read(tsm_tls* t, uint8_t* p, size_t n)
{
    __try { return read_raw(t, p, n); }
    __except (tsm_filter(GetExceptionInformation())) { return TSM_FAULT; }
}
void tsm_last_fault(unsigned long* code, const void** addr, const void** ref)
{
    *code = tsm_fault_code;
    *addr = tsm_fault_addr;
    *ref = (const void*)&mbedtls_ssl_handshake;
}
#else
int tsm_tls_handshake(tsm_tls* t) { return mbedtls_ssl_handshake(&t->ssl); }
int tsm_tls_read(tsm_tls* t, uint8_t* p, size_t n) { return mbedtls_ssl_read(&t->ssl, p, n); }
void tsm_last_fault(unsigned long* code, const void** addr, const void** ref) { *code = 0; *addr = 0; *ref = 0; }
#endif
int tsm_tls_write(tsm_tls* t, const uint8_t* p, size_t n) { return mbedtls_ssl_write(&t->ssl, p, n); }

void tsm_tls_free(tsm_tls* t) {
    if (!t) return;
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    free(t);
}

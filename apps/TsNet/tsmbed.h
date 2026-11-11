/* mbedTLS behind a plain C interface: VS2013 cannot compile mbedTLS' PSA headers as C++. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tsm_rng)(uint8_t* out, size_t n);
typedef int (*tsm_send)(void* ctx, const unsigned char* p, size_t n);
typedef int (*tsm_recv)(void* ctx, unsigned char* p, size_t n);

#define TSM_WANT_READ (-0x6900)    /* MBEDTLS_ERR_SSL_WANT_READ */
#define TSM_WANT_WRITE (-0x6880)   /* MBEDTLS_ERR_SSL_WANT_WRITE */
#define TSM_EOF (-0x7280)          /* MBEDTLS_ERR_SSL_CONN_EOF */
#define TSM_INTERNAL (-0x6C00)     /* MBEDTLS_ERR_SSL_INTERNAL_ERROR */
#define TSM_NEW_TICKET (-0x7B00)   /* MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET */
#define TSM_FAULT (-0x7F7F)        /* ours: a processor fault inside mbedTLS, see tsm_last_fault */

/* X25519 with a clamped scalar; pub NULL = base point. 0 on success. */
int tsm_x25519(uint8_t out[32], const uint8_t priv[32], const uint8_t* pub, tsm_rng rng);
void tsm_aead_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
                   const uint8_t* ad, size_t adn, uint8_t* out);
int tsm_aead_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in, size_t n,
                  const uint8_t* ad, size_t adn, uint8_t* out);
void tsm_poly1305(const uint8_t key[32], const uint8_t* in, size_t n, uint8_t tag[16]);

/* TLS 1.2 client without certificate checks */
typedef struct tsm_tls tsm_tls;
tsm_tls* tsm_tls_new(const char* host, void* ctx, tsm_send send, tsm_recv recv, tsm_rng rng);
int tsm_tls_handshake(tsm_tls* t);
int tsm_tls_read(tsm_tls* t, uint8_t* p, size_t n);
int tsm_tls_write(tsm_tls* t, const uint8_t* p, size_t n);
void tsm_tls_free(tsm_tls* t);
/* the last fault caught: exception code, where it happened, and the address of mbedtls_ssl_handshake for the map */
void tsm_last_fault(unsigned long* code, const void** addr, const void** ref);

#ifdef __cplusplus
}
#endif

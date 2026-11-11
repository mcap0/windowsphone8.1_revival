// WP8.1: no persistent PSA key storage (it needs MoveFileExA, not available to phone apps)
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

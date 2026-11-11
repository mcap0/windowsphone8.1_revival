# TgNative: client Telegram nativo per Windows Phone 8.1 (C++11)

- `core/`: logica MTProto portabile (C++11, niente librerie esterne oltre mbedTLS).
- `tests/`: test eseguibili sul PC (`tools/gptcpp apps/TgNative <round> <task>`).
- `third_party/mbedtls/`: mbedTLS 3.6.2 (Apache-2.0 / GPL-2.0-or-later), copia dei sorgenti `library/` e `include/`.
- `build/libmbedtls.a`: libreria mbedTLS per il PC. Ricostruzione: compilare `third_party/mbedtls/library/*.c`
  con `gcc -std=c99 -Iinclude -Ilibrary` e archiviare con `ar rcs build/libmbedtls.a *.o`.

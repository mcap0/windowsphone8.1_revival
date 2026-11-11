#!/bin/sh
# PC build of Tests/https_test (Core/Https.cpp + TsNet TLS + mbedTLS of TgNative)
cd "$(dirname "$0")"
TS=../../TsNet
MB=../../TgNative/third_party/mbedtls
gcc -c -O1 -I$MB/include -I$MB/library $TS/tsmbed.c -o /tmp/tsmbed_https.o && \
g++ -std=c++11 -O1 -I../Core -I$TS -I$MB/include https_test.cpp ../Core/Https.cpp ../Core/Online.cpp $TS/tsio.cpp \
    $TS/tscrypto.cpp $TS/tsjson.cpp /tmp/tsmbed_https.o ../../TgNative/build/libmbedtls.a -lpthread -o /tmp/https_test && echo built

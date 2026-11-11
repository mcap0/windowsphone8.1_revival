#!/bin/sh
# PC build of the remote core + TsNet TLS (mbedTLS of TgNative): apps/Telecomando/pc/remote_test
cd "$(dirname "$0")"
TS=../../TsNet
MB=../../TgNative/third_party/mbedtls
gcc -c -O1 -I$MB/include -I$MB/library $TS/tsmbed.c -o /tmp/tsmbed_$$.o && \
g++ -std=c++11 -O1 -I$TS -I$MB/include ../Telecomando/Core/TvRemote.cpp remote_test.cpp $TS/tsio.cpp $TS/tscrypto.cpp \
    /tmp/tsmbed_$$.o ../../TgNative/build/libmbedtls.a -lpthread -o remote_test && echo built remote_test
rm -f /tmp/tsmbed_$$.o

#!/bin/bash
# build.sh [tests|<live/x.cpp>] — PC build of the portable core with g++ (C++11, like VS2013) + mbedTLS + zlib + opus.
# build/libopus.a: third_party/opus sources.txt without arm/, see PROGRESS.md.
cd "$(dirname "$0")"
what=${1:-tests}
src=(core/*.cpp)
if [ "$what" = tests ]; then src+=(tests/*.cpp); out=build/tests; else src+=("$what"); out=build/$(basename "$what" .cpp); fi
O=third_party/opus
g++ -std=c++11 -O1 -g -Wall -Wno-unused -I. -Icore -Ithird_party/mbedtls/include -Ithird_party/zlib \
    -Ibuild/pcinc -Ibuild/pcinc/ogg -I$O/include -I$O/opusfile "${src[@]}" \
    build/libmbedtls.a build/libz.a build/libopus.a -lpthread -o "$out" && echo "built $out"

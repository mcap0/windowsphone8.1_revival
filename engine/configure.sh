#!/usr/bin/env bash
# configure.sh — configure WebKit (engine/webkit, tag + patches) for the WP8.1 port into out/webkit, or into
# $ENGINE_OUT (a variant build, e.g. out/webkit-lto with ENGINE_CMAKE_ARGS=-DWP81_LTO=ON; more cmake arguments after
# the script name).
set -Eeuo pipefail
engine=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out=${ENGINE_OUT:-$engine/out/webkit}
mkdir -p "$out" "$engine/logs"
# The dependencies are static libraries: their headers must not declare dllimport.
static_defs="-DU_STATIC_IMPLEMENTATION -DLIBXML_STATIC -DLIBXSLT_STATIC -DLIBEXSLT_STATIC -DCURL_STATICLIB -DPSL_STATIC"
cmake -G Ninja -S "$engine/webkit" -B "$out" \
    -DCMAKE_TOOLCHAIN_FILE="$engine/cmake/wp81-arm.cmake" \
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
    -DPORT=Win -DCMAKE_BUILD_TYPE=Release -DWP81_PORT_DIR="$engine/port" -DCMAKE_PREFIX_PATH="$engine/out/deps" \
    -DDEVELOPER_MODE=OFF -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_MINIBROWSER=OFF \
    -DENABLE_WEBDRIVER=OFF -DUSE_LCMS=OFF -DUSE_JPEGXL=OFF \
    -DENABLE_WEBGL=OFF -DENABLE_CONTENT_EXTENSIONS=ON \
    -DWP81_EXTRA_C_FLAGS="$static_defs ${ENGINE_EXTRA_FLAGS:-}" -DWP81_EXTRA_CXX_FLAGS="$static_defs ${ENGINE_EXTRA_FLAGS:-}" \
    ${ENGINE_CMAKE_ARGS:-} "$@"

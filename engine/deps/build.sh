#!/usr/bin/env bash
# deps/build.sh [name...] — build the engine's third-party libraries for WP8.1 ARM32 (static, /MT, Krait) into
# out/deps with the toolchain in cmake/wp81-arm.cmake. Without names: all of them, in dependency order.
# Sources come from deps/fetch.sh. One log per library: logs/deps-<name>.log.
set -Eeuo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
engine=$(dirname -- "$here")
src=$here/src
build=$engine/out/deps-build
prefix=$engine/out/deps
logs=$engine/logs
toolchain=$engine/cmake/wp81-arm.cmake
mkdir -p "$build" "$prefix/lib" "$prefix/include" "$logs"

ALL=(zlib brotli nghttp2 libressl libpsl curl icu harfbuzz libjpeg-turbo libpng libwebp libxml2 libxslt sqlite woff2)

# Common flags for C libraries: Windows 8.1 API level, function/data sections so the final DLL can drop unused code.
c_flags="-D_WIN32_WINNT=0x0603 -DWINVER=0x0603 /Gy /Gw"

cmake_dep() {
    local name=$1 dir=$2
    shift 2
    rm -rf "$build/$name"
    cmake -G Ninja -S "$dir" -B "$build/$name" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_PREFIX_PATH="$prefix" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DWP81_EXTRA_C_FLAGS="$c_flags" -DWP81_EXTRA_CXX_FLAGS="$c_flags" "$@"
    cmake --build "$build/$name"
    cmake --install "$build/$name"
}

build_zlib() {
    cmake_dep zlib "$src/zlib" -DZLIB_BUILD_SHARED=OFF -DZLIB_BUILD_STATIC=ON -DZLIB_BUILD_TESTING=OFF
    # zlib 1.3.2 names the static library zs.lib; FindZLIB and curl look for zlib.lib.
    cp "$prefix/lib/zs.lib" "$prefix/lib/zlib.lib"
}

build_brotli() {
    cmake_dep brotli "$src/brotli" -DBROTLI_BUILD_TOOLS=OFF -DBROTLI_DISABLE_TESTS=ON
}

build_nghttp2() {
    cmake_dep nghttp2 "$src/nghttp2" -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=ON -DENABLE_DOC=OFF
}

build_libressl() {
    cmake_dep libressl "$src/libressl" -DLIBRESSL_APPS=OFF -DLIBRESSL_TESTS=OFF -DENABLE_ASM=OFF \
        -DENABLE_NC=OFF -DUSE_STATIC_MSVC_RUNTIMES=ON
}

build_libpsl() {
    cmake_dep libpsl "$here/cmake/libpsl" -DPSL_SOURCE="$src/libpsl" -DPSL_VERSION="$(cat "$src/libpsl/.version")"
}

build_curl() {
    # nghttp2 is static too: without this its header declares every function dllimport.
    local c_flags="$c_flags -DNGHTTP2_STATICLIB"
    cmake_dep curl "$src/curl" -DBUILD_CURL_EXE=OFF -DBUILD_STATIC_LIBS=ON -DCURL_STATICLIB=ON \
        -DCURL_USE_OPENSSL=ON -DOPENSSL_ROOT_DIR="$prefix" -DOPENSSL_USE_STATIC_LIBS=ON -DUSE_NGHTTP2=ON \
        -DCURL_BROTLI=ON -DCURL_ZLIB=ON -DCURL_ZSTD=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF \
        -DCURL_USE_LIBSSH=OFF -DUSE_LIBIDN2=OFF -DUSE_WIN32_IDN=OFF -DCURL_USE_SCHANNEL=OFF -DCURL_WINDOWS_SSPI=OFF \
        -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DCURL_DISABLE_KERBEROS_AUTH=ON -DCURL_DISABLE_NEGOTIATE_AUTH=ON \
        -DCURL_DISABLE_AWS=ON -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_DICT=ON \
        -DCURL_DISABLE_GOPHER=ON -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_POP3=ON -DCURL_DISABLE_SMTP=ON \
        -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_MQTT=ON -DCURL_DISABLE_SMB=ON -DUSE_ECH=OFF -DUSE_HTTPSRR=OFF \
        -DENABLE_CURL_MANUAL=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DPICKY_COMPILER=OFF \
        -DENABLE_THREADED_RESOLVER=ON -DCURL_CA_BUNDLE=none -DCURL_CA_PATH=none
}

build_icu() {
    cmake_dep icu "$here/cmake/icu" -DICU_SOURCE="$src/icu/source"
    cp "$src/icu-data/icudt77l.dat" "$prefix/lib/"
}

build_harfbuzz() {
    cmake_dep harfbuzz "$src/harfbuzz" -DHB_HAVE_ICU=ON -DHB_HAVE_FREETYPE=OFF -DHB_HAVE_GLIB=OFF \
        -DHB_HAVE_GOBJECT=OFF -DHB_HAVE_CAIRO=OFF -DHB_HAVE_DIRECTWRITE=OFF -DHB_HAVE_GDI=OFF -DHB_HAVE_UNISCRIBE=OFF \
        -DHB_HAVE_GRAPHITE=OFF -DHB_BUILD_SUBSET=OFF -DHB_BUILD_UTILS=OFF -DHB_BUILD_RASTER=OFF -DHB_BUILD_VECTOR=OFF \
        -DHB_BUILD_GPU=OFF -DICU_ROOT="$prefix" -DWP81_EXTRA_CXX_FLAGS="$c_flags -DHB_NO_PRAGMA_GCC_DIAGNOSTIC_ERROR /I$prefix/include"
}

build_libjpeg-turbo() {
    cmake_dep libjpeg-turbo "$src/libjpeg-turbo" -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
        -DWITH_SIMD=ON -DNEON_INTRINSICS=ON -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
}

build_libpng() {
    cmake_dep libpng "$src/libpng" -DPNG_SHARED=OFF -DPNG_STATIC=ON -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
        -DPNG_FRAMEWORK=OFF -DPNG_ARM_NEON=off -DZLIB_ROOT="$prefix" -DSKIP_INSTALL_FILES=ON
}

build_libwebp() {
    cmake_dep libwebp "$src/libwebp" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
        -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
        -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF -DWEBP_BUILD_LIBWEBPMUX=ON -DWEBP_ENABLE_SIMD=ON
}

build_libxml2() {
    # Static inside webkit.dll: per-thread state is freed by xmlDllMain, called from the port's DllMain on thread
    # detach (the default static mode needs RegisterWaitForSingleObject, which WP8.1 apps don't have).
    local c_flags="$c_flags -DLIBXML_STATIC_FOR_DLL"
    cmake_dep libxml2 "$src/libxml2" -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_ICU=OFF -DLIBXML2_WITH_LZMA=OFF \
        -DLIBXML2_WITH_ZLIB=OFF -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_TESTS=OFF \
        -DLIBXML2_WITH_HTTP=OFF -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_THREADS=ON
}

build_libxslt() {
    # Static: without these its own sources mark every function dllexport, and webkit.dll would export them.
    local c_flags="$c_flags -DLIBXSLT_STATIC -DLIBEXSLT_STATIC -DLIBXML_STATIC"
    cmake_dep libxslt "$src/libxslt" -DLIBXSLT_WITH_PYTHON=OFF -DLIBXSLT_WITH_CRYPTO=OFF -DLIBXSLT_WITH_PROGRAMS=OFF \
        -DLIBXSLT_WITH_TESTS=OFF -DLIBXSLT_WITH_MODULES=OFF
}

build_sqlite() {
    cmake_dep sqlite "$here/cmake/sqlite" -DSQLITE_SOURCE="$src/sqlite"
}

build_woff2() {
    cmake_dep woff2 "$src/woff2" -DNOISY_LOGGING=OFF -DCANONICAL_PREFIXES=ON \
        -DBROTLIDEC_INCLUDE_DIRS="$prefix/include" -DBROTLIDEC_LIBRARIES="$prefix/lib/brotlidec.lib;$prefix/lib/brotlicommon.lib" \
        -DBROTLIENC_INCLUDE_DIRS="$prefix/include" -DBROTLIENC_LIBRARIES="$prefix/lib/brotlienc.lib;$prefix/lib/brotlicommon.lib"
}

names=("$@")
(( ${#names[@]} )) || names=("${ALL[@]}")
failed=()
for name in "${names[@]}"; do
    start=$SECONDS
    if "build_$name" > "$logs/deps-$name.log" 2>&1; then
        echo "ok     $name ($((SECONDS - start)) s)"
    else
        echo "FAILED $name ($((SECONDS - start)) s) — see logs/deps-$name.log"
        failed+=("$name")
    fi
done
# Conventional names for the static libraries, so CMake's Find modules (WebKit's configure) find them.
for pair in libxml2s:libxml2 libxml2s:xml2 libxslts:libxslt libxslts:xslt libexslts:libexslt libexslts:exslt \
            jpeg-static:jpeg libpng16_static:libpng16 libpng16_static:png libwebp:webp libwebpdemux:webpdemux \
            libsharpyuv:sharpyuv; do
    from=${pair%%:*} to=${pair##*:}
    [[ -f $prefix/lib/$from.lib ]] && cp "$prefix/lib/$from.lib" "$prefix/lib/$to.lib"
done

# Every library must use the static CRT (/MT): a /MD object asks for MSVCRT and breaks the final link.
for lib in "$prefix"/lib/*.lib; do
    if strings -n 6 "$lib" | grep -q -i -E 'DEFAULTLIB:"?MSVCRT'; then
        echo "WRONG CRT (/MD) in $(basename "$lib")"
        failed+=("$(basename "$lib")")
    fi
done
(( ${#failed[@]} == 0 )) || { echo "failed: ${failed[*]}"; exit 1; }

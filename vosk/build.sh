#!/usr/bin/env bash
# vosk/build.sh [step...] — Vosk (vosk-api + Kaldi + OpenFst + OpenBLAS) for WP8.1 ARM32 with the engine's toolchain
# (engine/cmake/wp81-arm.cmake: clang-cl + lld-link, static CRT, phone import libs). Steps: openblas, vosk.
# Sources in vosk/deps/src (git clones, see PLAN.md §59.3). Output in vosk/out.
set -Eeuo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
src=$here/deps/src
out=$here/out
toolchain=$here/../engine/cmake/wp81-arm.cmake
mkdir -p "$out/logs"
c_flags="-D_WIN32_WINNT=0x0603 -DWINVER=0x0603 /Gy /Gw"

build_openblas() {
    # plain C kernels (ARMV5: all C, generic 2x2 gemm; the ARMV6/7 ones are GNU assembly), compiled for Krait;
    # LAPACK translated to C, no threads
    [ -n "${KEEP:-}" ] || rm -rf "$out/openblas-build"
    cmake -G Ninja -S "$src/openblas" -B "$out/openblas-build" -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$out/prefix" -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_STATIC_LIBS=ON -DBUILD_TESTING=OFF -DTARGET=ARMV5 -DNOFORTRAN=ON -DC_LAPACK=ON \
        -DUSE_THREAD=OFF -DUSE_LOCKING=ON -DDYNAMIC_ARCH=OFF -DBUILD_BFLOAT16=OFF -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DWP81_EXTRA_C_FLAGS="$c_flags" -DWP81_EXTRA_CXX_FLAGS="$c_flags"
    cmake --build "$out/openblas-build"
    cmake --install "$out/openblas-build"
}

build_vosk() {
    [ -n "${KEEP:-}" ] || rm -rf "$out/vosk-build"
    cmake -G Ninja -S "$here" -B "$out/vosk-build" -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded \
        -DWP81_EXTRA_C_FLAGS="$c_flags" -DWP81_EXTRA_CXX_FLAGS="$c_flags"
    cmake --build "$out/vosk-build" -- -k 0
}

# host (Linux) build of the same patched sources, to debug on the PC: out/host/vosktest <model> <wav>
build_host() {
    local b=$out/host
    mkdir -p "$b"
    [ -f "$b/prefix/lib/libopenblas.a" ] || {
        cmake -G Ninja -S "$src/openblas" -B "$b/openblas" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$b/prefix" \
            -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DNOFORTRAN=ON -DC_LAPACK=ON -DUSE_THREAD=OFF -DUSE_LOCKING=ON -DDYNAMIC_ARCH=OFF
        cmake --build "$b/openblas" && cmake --install "$b/openblas"; }
    cmake -G Ninja -S "$here" -B "$b/vosk" -DCMAKE_BUILD_TYPE=Release -DHOST=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build "$b/vosk"
}

for step in "${@:-openblas vosk}"; do
    for s in $step; do
        echo "== $s"
        "build_$s" > "$out/logs/$s.log" 2>&1 || { echo "FAILED: see $out/logs/$s.log"; tail -30 "$out/logs/$s.log"; exit 1; }
    done
done

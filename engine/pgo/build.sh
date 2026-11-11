#!/usr/bin/env bash
# pgo/build.sh — the PGO runtime for the phone: out/pgo-rt/clang_rt.profile.lib, the library the objects compiled with
# -fprofile-generate ask for. compiler-rt's counters, value profiler and writer from out/llvm-src (the same LLVM as
# the distribution's clang, so the raw profile format matches llvm-profdata), plus wp81profile.c.
set -Eeuo pipefail
engine=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
src=$engine/out/llvm-src/compiler-rt
out=$engine/out/pgo-rt
msvc=$engine/sysroot/msvc
mkdir -p "$out"
objects=()
sources=()
for file in InstrProfiling InstrProfilingBuffer InstrProfilingInternal InstrProfilingMerge InstrProfilingPlatformWindows \
    InstrProfilingValue InstrProfilingWriter InstrProfilingVersionVar InstrProfilingNameVar; do
    sources+=("$src/lib/profile/$file.c")
done
sources+=("$engine/pgo/wp81profile.c")
for source in "${sources[@]}"; do
    object=$out/$(basename "${source%.c}").obj
    clang-cl --target=thumbv7-pc-windows-msvc /clang:-mcpu=krait /O2 /MT /c /nologo -w \
        -imsvc "$msvc/crt/include" -imsvc "$msvc/sdk/include/ucrt" -imsvc "$msvc/sdk/include/um" -imsvc "$msvc/sdk/include/shared" \
        -D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 -DCOMPILER_RT_HAS_ATOMICS=1 \
        -I "$src/lib/profile" -I "$src/include" "$source" "/Fo$object"
    objects+=("$object")
done
rm -f "$out/clang_rt.profile.lib"
llvm-lib /nologo "/out:$out/clang_rt.profile.lib" "${objects[@]}"
echo "$out/clang_rt.profile.lib"

# CMake toolchain: Windows Phone 8.1 ARM32 (Thumb-2, Krait), cross-compiled on Linux with clang-cl + lld-link.
#
# Headers and the static CRT/STL come from the MSVC 14.44 + SDK 10.0.22621 ARM sysroot (sysroot/msvc, made with xwin).
# Imports never go through the SDK's umbrella libraries: they resolve against import libraries generated from the
# phone's own DLLs (sysroot/phone, tools/implib.py), so a function the phone lacks is a link error here.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 6.3)
# Lowercase: several projects (LibreSSL...) match the processor name case-sensitively against "arm".
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_ASM_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_C_COMPILER_TARGET thumbv7-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET thumbv7-pc-windows-msvc)

get_filename_component(WP81_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(WP81_SYSROOT "${WP81_ENGINE_ROOT}/sysroot")
set(WP81_PHONE_LIBS "${WP81_SYSROOT}/phone")
set(_msvc "${WP81_SYSROOT}/msvc")

# WP81 is both a CMake switch (OptionsWin.cmake) and a C/C++ define (the WP81 patches in WebKit and the port).
set(WP81 ON CACHE BOOL "Build for Windows Phone 8.1" FORCE)

set(_wp81_flags
    "--target=thumbv7-pc-windows-msvc /clang:-mcpu=krait /clang:-ffp-contract=off"
    "-imsvc ${_msvc}/crt/include -imsvc ${_msvc}/sdk/include/ucrt -imsvc ${_msvc}/sdk/include/um"
    "-imsvc ${_msvc}/sdk/include/shared -imsvc ${_msvc}/sdk/include/winrt"
    "-D_ARM_WINAPI_PARTITION_DESKTOP_SDK_AVAILABLE=1 -DWP81=1 /FI${WP81_ENGINE_ROOT}/cmake/wp81-compat.h")
string(REPLACE ";" " " _wp81_flags "${_wp81_flags}")
# Callers add flags with WP81_EXTRA_C_FLAGS / WP81_EXTRA_CXX_FLAGS: setting CMAKE_C_FLAGS directly would replace these.
# WP81_LTO (a variant build, engine/out/webkit-lto): ThinLTO across the whole engine. Its own archiver (cmake/lto-lib) and
# an LTO cache, so a relink after a small change only recompiles the modules that changed.
if (WP81_LTO)
    string(APPEND _wp81_flags " -flto=thin")
    # lld built from engine/out/llvm-src with the static relocation model on ARM COFF (lld/COFF/LTO.cpp): the distribution's
    # generates LTO code as PIC, which COFF on ARM cannot express.
    set(CMAKE_LINKER "${CMAKE_CURRENT_LIST_DIR}/../out/lld-build/bin/lld-link")
    set(CMAKE_AR "${CMAKE_CURRENT_LIST_DIR}/lto-lib")
endif ()
# WP81_PGO (PLAN.md §74, engine/pgo): "gen" = the instrumented engine (out/webkit-pgo), which counts what runs and
# writes the counts on the "pgo" command; "use" = compile with the profile gathered on the phone (pgo/webkit.profdata):
# inlining, block layout and hot/cold decisions from real counts, and the linker lays functions out by the call graph.
if (WP81_PGO STREQUAL "gen")
    string(APPEND _wp81_flags " -fprofile-generate -DWP81_PGO=1")
elseif (WP81_PGO STREQUAL "use")
    string(APPEND _wp81_flags " -fprofile-use=${WP81_ENGINE_ROOT}/pgo/webkit.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-backend-plugin")
endif ()
set(CMAKE_C_FLAGS_INIT "${_wp81_flags} ${WP81_EXTRA_C_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_wp81_flags} ${WP81_EXTRA_CXX_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${_wp81_flags}")
set(CMAKE_RC_FLAGS_INIT "-I ${_msvc}/sdk/include/um -I ${_msvc}/sdk/include/shared -I ${_msvc}/sdk/include/ucrt")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES WP81_EXTRA_C_FLAGS WP81_EXTRA_CXX_FLAGS WP81_LTO WP81_PGO)

# Static CRT everywhere (/MT): WP8.1 has no UCRT DLL. Projects whose cmake_minimum_required predates CMP0091 need
# -DCMAKE_POLICY_DEFAULT_CMP0091=NEW on the command line (policy defaults are read before this file; deps/build.sh
# passes it). CMake's own compiler checks run in Release: the sysroot has no debug runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# The phone's import libraries come first in the search path, so "ws2_32.lib", "bcrypt.lib" or even "kernel32.lib"
# (an alias of KERNELBASE.DLL there) named by a dependency resolve to what the phone really exports.
set(_wp81_link
    "/machine:arm /libpath:${WP81_PHONE_LIBS} /libpath:${_msvc}/crt/lib/arm /libpath:${_msvc}/sdk/lib/ucrt/arm"
    "/libpath:${_msvc}/sdk/lib/um/arm /nodefaultlib:user32.lib /nodefaultlib:advapi32.lib /nodefaultlib:ole32.lib"
    "/nodefaultlib:shell32.lib /nodefaultlib:gdi32.lib")
string(REPLACE ";" " " _wp81_link "${_wp81_link}")
if (WP81_LTO)
    # ARM's global merge pass turns globals into offsets of one merged block, aliases included; with LTO it reaches
    # external globals, and an alias at an offset is something ARM COFF objects cannot express ("Cannot represent
    # this expression" at link time).
    string(APPEND _wp81_link " /lldltocache:${WP81_ENGINE_ROOT}/out/lto-cache /mllvm:-arm-global-merge=false")
endif ()
if (WP81_PGO STREQUAL "gen")
    # clang_rt.profile.lib, asked for by every instrumented object (pgo/build.sh).
    string(APPEND _wp81_link " /libpath:${WP81_ENGINE_ROOT}/out/pgo-rt")
endif ()
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_wp81_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_wp81_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_wp81_link}")

# Replace CMake's default Windows libraries (kernel32, user32, gdi32, ...: not on the phone) with the phone's.
set(_wp81_std_libs "${WP81_PHONE_LIBS}/kernelbase.lib ${WP81_PHONE_LIBS}/ntdll.lib")
set(CMAKE_C_STANDARD_LIBRARIES "${_wp81_std_libs}" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "${_wp81_std_libs}" CACHE STRING "" FORCE)

# Libraries and headers are searched only in our dependency prefix.
set(WP81_DEPS_PREFIX "${WP81_ENGINE_ROOT}/out/deps" CACHE PATH "Prefix of the WP8.1 dependency builds")
set(CMAKE_FIND_ROOT_PATH "${WP81_DEPS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# CMake toolchain: Linux ARMv7 hard-float (Thumb-2), for a jsc shell of our JavaScriptCore that runs on this PC under
# qemu-arm (engine/tools/jsclinux): the 32-bit JIT and WebAssembly tested without the phone. Sysroot = Debian trixie
# armhf packages extracted into engine/out/jsc-linux/sysroot.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET armv7a-linux-gnueabihf)
set(CMAKE_CXX_COMPILER_TARGET armv7a-linux-gnueabihf)
set(CMAKE_ASM_COMPILER_TARGET armv7a-linux-gnueabihf)
get_filename_component(_engine "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_SYSROOT "${_engine}/out/jsc-linux/sysroot")
# Krait is a Cortex-A15 class core: VFPv4, NEON, integer divide.
set(_flags "-mthumb -mcpu=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=hard -ffp-contract=off -DWP81_LINUX_TEST=1")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

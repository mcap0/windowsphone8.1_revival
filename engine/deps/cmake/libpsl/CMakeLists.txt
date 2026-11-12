# libpsl with its built-in public suffix list (suffixes_dafsa.h ships in the release tarball), no IDN library.
cmake_minimum_required(VERSION 3.20)
project(psl C)

set(PSL_SOURCE "" CACHE PATH "libpsl source directory")
set(PSL_VERSION "" CACHE STRING "libpsl version")
add_library(psl STATIC "${PSL_SOURCE}/src/psl.c" "${PSL_SOURCE}/src/lookup_string_in_fixed_set.c")
target_include_directories(psl PUBLIC "$<BUILD_INTERFACE:${PSL_SOURCE}/include>" "$<INSTALL_INTERFACE:include>"
    PRIVATE "${PSL_SOURCE}/src")
target_compile_definitions(psl PUBLIC PSL_STATIC PRIVATE ENABLE_BUILTIN "PACKAGE_VERSION=\"${PSL_VERSION}\"")
install(TARGETS psl ARCHIVE DESTINATION lib)
install(FILES "${PSL_SOURCE}/include/libpsl.h" DESTINATION include)

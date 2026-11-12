# ICU 77 common + i18n as static libraries. ICU has no CMake build; its source lists (sources.txt) are the
# reference. Data is not compiled in (stubdata): the port points ICU at icudt77l.dat in the app package.
cmake_minimum_required(VERSION 3.20)
project(icu C CXX)

set(ICU_SOURCE "" CACHE PATH "icu4c 'source' directory")
set(CMAKE_CXX_STANDARD 17)

file(STRINGS "${ICU_SOURCE}/common/sources.txt" common_sources)
file(STRINGS "${ICU_SOURCE}/i18n/sources.txt" i18n_sources)
list(TRANSFORM common_sources PREPEND "${ICU_SOURCE}/common/")
list(TRANSFORM i18n_sources PREPEND "${ICU_SOURCE}/i18n/")

add_library(icuuc STATIC ${common_sources})
target_include_directories(icuuc PUBLIC "$<BUILD_INTERFACE:${ICU_SOURCE}/common>" "$<INSTALL_INTERFACE:include>")
target_compile_definitions(icuuc PRIVATE U_COMMON_IMPLEMENTATION PUBLIC U_STATIC_IMPLEMENTATION)

add_library(icuin STATIC ${i18n_sources})
target_include_directories(icuin PRIVATE "${ICU_SOURCE}/i18n")
target_compile_definitions(icuin PRIVATE U_I18N_IMPLEMENTATION)
target_link_libraries(icuin PUBLIC icuuc)

add_library(icudt STATIC "${ICU_SOURCE}/stubdata/stubdata.cpp")
target_include_directories(icudt PRIVATE "${ICU_SOURCE}/common")
target_compile_definitions(icudt PRIVATE U_STATIC_IMPLEMENTATION)

install(TARGETS icuuc icuin icudt ARCHIVE DESTINATION lib)
install(DIRECTORY "${ICU_SOURCE}/common/unicode/" "${ICU_SOURCE}/i18n/unicode/" DESTINATION include/unicode)

# Vosk for WP8.1 ARM32: OpenFst + the Kaldi modules Vosk links + vosk-api, as one vosk.dll exporting the C API
# (vosk_api.h). Built by vosk/build.sh with engine/cmake/wp81-arm.cmake; OpenBLAS (C kernels, C LAPACK) from out/prefix.
cmake_minimum_required(VERSION 3.20)
project(vosk_wp81 C CXX)
set(CMAKE_CXX_STANDARD 17)
set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/deps/src)
set(PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/out/prefix)
option(HOST "Linux build of the same sources, for debugging on the PC" OFF)
if (HOST)
    set(PREFIX ${CMAKE_CURRENT_SOURCE_DIR}/out/host/prefix)
endif ()

add_compile_definitions(
    FST_NO_DYNAMIC_LINKING HAVE_OPENBLAS=1 KALDI_DOUBLEPRECISION=0 _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
# clang-cl: Kaldi and OpenFst are GCC/MSVC code with many warnings that do not matter here
# no C++ exceptions (LLVM has no EH for Windows ARM32): vosk/noexcept.py rewrote try/catch/throw
if (HOST)
    add_compile_options(-fno-exceptions -O2 -g -w -include ${CMAKE_CURRENT_SOURCE_DIR}/wp81-vosk.h)
else ()
    add_compile_options(/EHs-c- /O2 -Wno-everything /FI${CMAKE_CURRENT_SOURCE_DIR}/wp81-vosk.h)
endif ()
add_compile_definitions(WP81_NO_FORTRAN_STRLEN _HAS_EXCEPTIONS=0)

include_directories(${CMAKE_CURRENT_SOURCE_DIR} ${SRC}/openfst/src/include ${SRC}/kaldi/src ${PREFIX}/include/openblas ${SRC}/vosk-api/src)

# OpenFst: the library, n-gram and look-ahead FSTs (Vosk's graph is HCLr + Gr with olabel look-ahead)
file(GLOB FST_SRC ${SRC}/openfst/src/lib/*.cc ${SRC}/openfst/src/extensions/ngram/*.cc
     ${SRC}/openfst/src/extensions/lookahead/*.cc)
add_library(fst STATIC ${FST_SRC})

# Kaldi: the modules in vosk-api/src/Makefile's link line (+ chain, needed by nnet3)
set(KALDI_DIRS base matrix util feat lat decoder fstext hmm nnet3 ivector online2 cudamatrix lm rnnlm transform tree
    gmm chain)
set(KALDI_SRC)
foreach(d ${KALDI_DIRS})
    file(GLOB s ${SRC}/kaldi/src/${d}/*.cc)
    list(FILTER s EXCLUDE REGEX "-test\\.cc$")
    list(APPEND KALDI_SRC ${s})
endforeach()
add_library(kaldi STATIC ${KALDI_SRC})

set(VOSK_SRC recognizer.cc language_model.cc model.cc spk_model.cc vosk_api.cc postprocessor.cc)
list(TRANSFORM VOSK_SRC PREPEND ${SRC}/vosk-api/src/)
if (HOST)
    add_executable(vosktest ${VOSK_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/wp81-vosk.cc ${CMAKE_CURRENT_SOURCE_DIR}/msym.cc ${CMAKE_CURRENT_SOURCE_DIR}/vosktest.cc)
    target_link_libraries(vosktest PRIVATE kaldi fst ${PREFIX}/lib64/libopenblas.a pthread m)
    # whole fst archive: the lookahead and ngram types register themselves from static objects
    add_executable(fstalign ${CMAKE_CURRENT_SOURCE_DIR}/fstalign.cc ${CMAKE_CURRENT_SOURCE_DIR}/msym.cc)
    target_link_libraries(fstalign PRIVATE -Wl,--whole-archive fst -Wl,--no-whole-archive dl)
    # HCLr o Gr expanded into a static HCLG (PLAN.md §59.5)
    add_executable(mkhclg ${CMAKE_CURRENT_SOURCE_DIR}/mkhclg.cc ${CMAKE_CURRENT_SOURCE_DIR}/wp81-vosk.cc)
    target_link_libraries(mkhclg PRIVATE kaldi -Wl,--whole-archive fst -Wl,--no-whole-archive ${PREFIX}/lib64/libopenblas.a dl pthread m)
else ()
    add_library(vosk SHARED ${VOSK_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/wp81-vosk.cc ${CMAKE_CURRENT_SOURCE_DIR}/msym.cc ${CMAKE_CURRENT_SOURCE_DIR}/vosk.def)
    target_link_libraries(vosk PRIVATE kaldi fst ${PREFIX}/lib/openblas.lib)
endif ()

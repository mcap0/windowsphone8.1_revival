cmake_minimum_required(VERSION 3.8)
project(PerleCastCore CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(perlecast
    core/Util.cpp
    core/Xml.cpp
    core/Ssdp.cpp
    core/DlnaDevice.cpp
    core/Didl.cpp
    core/Soap.cpp
    core/Srt.cpp
    core/TsWriter.cpp
    core/TsMuxer.cpp
    core/ProxyHttp.cpp
    core/CastController.cpp
    core/CastQueue.cpp)

add_executable(perlecast_tests
    tests/main.cpp
    tests/ssdp_tests.cpp
    tests/dlna_soap_tests.cpp
    tests/srt_tests.cpp
    tests/ts_tests.cpp
    tests/proxyhttp_tests.cpp
    tests/controller_tests.cpp
    tests/queue_tests.cpp)

target_link_libraries(perlecast_tests perlecast)

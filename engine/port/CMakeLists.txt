# Lumia port of WebKit (WP8.1 ARM32). Added by Source/CMakeLists.txt when WP81 is on (WP81_PORT_DIR).
#
# Targets:
#   jsctest  - JavaScriptCore alone in a DLL, driven by apps/EngineTest (milestone 1a: JS and the JIT on the phone).
#   webkit   - the engine: WebCore + JSC + our port (clients, WebPage, compositor, loader), C API lumiaweb.h only.

# Win32 functions the phone lacks, provided to every port DLL (shims/wp81shims.c).
add_library(wp81shims STATIC shims/wp81shims.c shims/wp81barrier.c)

add_library(jsctest SHARED tests/jsctest.cpp)
target_include_directories(jsctest BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# WTF and bmalloc are object libraries: their objects are linked explicitly.
target_link_libraries(jsctest PRIVATE JavaScriptCore WTF bmalloc wp81shims)
target_compile_definitions(jsctest PRIVATE BUILDING_WITH_CMAKE=1 HAVE_CONFIG_H=1 STATICALLY_LINKED_WITH_JavaScriptCore STATICALLY_LINKED_WITH_WTF)
target_include_directories(jsctest PRIVATE ${CMAKE_BINARY_DIR} ${JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS_DIR} ${JavaScriptCore_FRAMEWORK_HEADERS_DIR} ${WTF_FRAMEWORK_HEADERS_DIR} ${bmalloc_FRAMEWORK_HEADERS_DIR})
set_target_properties(jsctest PROPERTIES OUTPUT_NAME jsctest)

# webkit.dll: everything static inside, exports only the lw_* functions (LW_BUILDING). Compiled with WebCore's own
# include paths and definitions; our folders come first, since WebCore has headers with the same names.
set(WP81_PORT_SOURCES
    BackForwardList.cpp
    Blocker.cpp
    ChromeClient.cpp
    Downloads.cpp
    EditorClient.cpp
    Authentication.cpp
    Engine.cpp
    Profiler.cpp
    FrameLoaderClient.cpp
    Permissions.cpp
    Pickers.cpp
    PlatformStrategies.cpp
    ScrollingTreeDispatcher.cpp
    Storage.cpp
    VisitedLinks.cpp
    WebPage.cpp
    compositor/AcceleratedSurface.cpp
    compositor/CompositingRunLoop.cpp
    compositor/CoordinatedSceneState.cpp
    compositor/FrameStats.cpp
    compositor/LayerTreeHost.cpp
    compositor/ScrollingCoordinatorCoordinated.cpp
    compositor/ThreadedCompositor.cpp
    compositor/ThreadedDisplayRefreshMonitor.cpp
    loader/WebResourceLoadScheduler.cpp
    loader/WebResourceLoadSchedulerErrors.cpp
    media/AudioOutput.cpp
    media/Demux.cpp
    media/MediaData.cpp
    media/MediaDecoder.cpp
    media/MediaPipeline.cpp
    media/MediaPlayerPrivateWP81.cpp
    media/VideoLayer.cpp
    media/VideoProbe.cpp
)
# localStorage, sessionStorage and IndexedDB: WebKitLegacy's in-process implementations, compiled from its tree.
set(WP81_LEGACY_STORAGE_DIR ${CMAKE_SOURCE_DIR}/Source/WebKitLegacy/Storage)
foreach (file InProcessIDBServer StorageAreaImpl StorageAreaSync StorageNamespaceImpl StorageSyncManager StorageThread
        StorageTracker WebDatabaseProvider WebStorageNamespaceProvider)
    list(APPEND WP81_PORT_SOURCES ${WP81_LEGACY_STORAGE_DIR}/${file}.cpp)
    # WebKitLegacy gets config.h from a forced prefix header (WebKitPrefix.h): ours instead.
    set_source_files_properties(${WP81_LEGACY_STORAGE_DIR}/${file}.cpp PROPERTIES COMPILE_OPTIONS "/FIconfig.h")
endforeach ()
# WebSockets: WebKitLegacy's in-process channel and its socket stream base class, the inspector hooks the channel calls
# (WebCore builds them only on Cocoa), and our curl stream under them.
set(WP81_LEGACY_SUPPORT_DIR ${CMAKE_SOURCE_DIR}/Source/WebKitLegacy/WebCoreSupport)
foreach (file WebSocketChannel SocketStreamHandle)
    list(APPEND WP81_PORT_SOURCES ${WP81_LEGACY_SUPPORT_DIR}/${file}.cpp)
    set_source_files_properties(${WP81_LEGACY_SUPPORT_DIR}/${file}.cpp PROPERTIES COMPILE_OPTIONS "/FIconfig.h")
endforeach ()
list(APPEND WP81_PORT_SOURCES
    ${CMAKE_SOURCE_DIR}/Source/WebCore/inspector/LegacyWebSocketInspectorInstrumentation.cpp
    SocketStreamHandleImplCurl.cpp)
# The viewport (<meta name=viewport>, layout width, initial and minimum scale) as Safari computes it: WebCore builds
# ViewportConfiguration only on Cocoa; WebPage.cpp drives it as WebKit2's WebPageIOS does.
list(APPEND WP81_PORT_SOURCES ${CMAKE_SOURCE_DIR}/Source/WebCore/page/ViewportConfiguration.cpp)
# HTTP disk cache (4d): WebKit2's network cache storage (record files, blobs shared through hard links, the shrink to
# capacity), compiled from its tree; what is kept and when it is fresh is ours (DiskCache.cpp), in front of curl
# (ResourceHandleCurl.cpp, WP81). networkcache/ has the one WebKit2 header the storage asks for beyond its own folder.
set(WP81_NETWORK_CACHE_DIR ${CMAKE_SOURCE_DIR}/Source/WebKit/NetworkProcess/cache)
foreach (file NetworkCacheBlobStorage NetworkCacheData NetworkCacheDataCurl NetworkCacheFileSystem NetworkCacheIOChannelCurl
        NetworkCacheKey NetworkCacheStorage)
    list(APPEND WP81_PORT_SOURCES ${WP81_NETWORK_CACHE_DIR}/${file}.cpp)
    set_source_files_properties(${WP81_NETWORK_CACHE_DIR}/${file}.cpp PROPERTIES COMPILE_OPTIONS "/FIconfig.h;/FINetworkCacheLogWP81.h")
endforeach ()
list(APPEND WP81_PORT_SOURCES DiskCache.cpp)
# JS bytecode cache (phase 5): JSC's own serialization behind WebCore's script source providers
# (WebCore/bindings/js/ScriptBytecodeCacheWP81.h); storage and policy here.
list(APPEND WP81_PORT_SOURCES BytecodeCache.cpp)
# WebKit's defaults for WebCore::Settings, from the same yaml WebCore's Settings are generated from.
set(WP81_PREFERENCES_YAML ${CMAKE_SOURCE_DIR}/Source/WTF/Scripts/Preferences/UnifiedWebPreferences.yaml)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/WebPreferencesDefaults.cpp
    DEPENDS ${WP81_ENGINE_ROOT}/tools/genpreferences.rb ${WP81_PREFERENCES_YAML}
    COMMAND ${Ruby_EXECUTABLE} ${WP81_ENGINE_ROOT}/tools/genpreferences.rb ${WP81_PREFERENCES_YAML} ${CMAKE_CURRENT_BINARY_DIR}/WebPreferencesDefaults.cpp
    VERBATIM)
list(APPEND WP81_PORT_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/WebPreferencesDefaults.cpp)

add_library(webkit SHARED ${WP81_PORT_SOURCES})
target_include_directories(webkit BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/compositor
    ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/media
    ${WP81_LEGACY_STORAGE_DIR}
    ${WP81_ENGINE_ROOT}/include)
target_include_directories(webkit PRIVATE $<TARGET_PROPERTY:WebCore,INCLUDE_DIRECTORIES>)
# After WebCore's: only for the socket stream and WebSocket channel headers, which WebCore does not have, and the
# network cache storage's.
target_include_directories(webkit PRIVATE ${WP81_LEGACY_SUPPORT_DIR} ${WP81_NETWORK_CACHE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/networkcache)
target_compile_definitions(webkit PRIVATE
    $<FILTER:$<TARGET_PROPERTY:WebCore,COMPILE_DEFINITIONS>,EXCLUDE,^BUILDING_WebCore$>
    LW_BUILDING STATICALLY_LINKED_WITH_WebCore BUILDING_WITH_CMAKE=1 HAVE_CONFIG_H=1)
# DirectWrite (fonts) and COMBASE are the system DLLs besides KERNELBASE, NTDLL, WS2_32, BCRYPT and ANGLE.
# COMBASE: WinRT activation (the ANGLE property set, compositor/AcceleratedSurface.cpp).
# Media (media/): the decoders through MFPLAT, sound through XAUDIO2_8 (import library of the phone's own DLL:
# engine/tools/implib.py ref/mainos/fs/Windows/system32/XAUDIO2_8.DLL -o engine/sysroot/phone); mfuuid = the GUIDs.
target_link_libraries(webkit PRIVATE WebCore PAL JavaScriptCore WTF bmalloc wp81shims dwrite combase mfplat mfuuid xaudio2_8 d3d11)
set_target_properties(webkit PROPERTIES OUTPUT_NAME webkit)
# Link map next to the DLL: crash offsets from the phone's log are symbolized with it (engine/tools/sym.py).
target_link_options(webkit PRIVATE /MAP:${CMAKE_BINARY_DIR}/bin/webkit.map)

# Every DLL of the port must load on the phone: imports are checked against the phone's own system DLLs.
foreach (target jsctest webkit)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${Python_EXECUTABLE} -I ${WP81_ENGINE_ROOT}/tools/checkimports.py $<TARGET_FILE:${target}>
            --package ${WP81_ENGINE_ROOT}/out/angle/bin
        VERBATIM)
endforeach ()

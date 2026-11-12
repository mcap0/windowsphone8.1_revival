# SQLite amalgamation as a static library for WebCore (IndexedDB, cookies, storage).
cmake_minimum_required(VERSION 3.20)
project(sqlite3 C)

set(SQLITE_SOURCE "" CACHE PATH "sqlite amalgamation directory")
add_library(sqlite3 STATIC "${SQLITE_SOURCE}/sqlite3.c")
target_include_directories(sqlite3 PUBLIC "$<BUILD_INTERFACE:${SQLITE_SOURCE}>" "$<INSTALL_INTERFACE:include>")
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_DQS=0
    SQLITE_WIN32_NO_ANSI SQLITE_WIN32_GETVERSIONEX=0 SQLITE_ENABLE_COLUMN_METADATA
    # clang has no SEH (__try) on 32-bit ARM Windows.
    SQLITE_OMIT_SEH)
install(TARGETS sqlite3 ARCHIVE DESTINATION lib)
install(FILES "${SQLITE_SOURCE}/sqlite3.h" "${SQLITE_SOURCE}/sqlite3ext.h" DESTINATION include)

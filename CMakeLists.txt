cmake_minimum_required(VERSION 3.20)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR "In-source builds are not allowed; configure into build/.")
endif()

project(frostlite LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "FrostLite targets Windows x64 only for now (see CONTRIBUTING.md).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

if(MSVC)
    # Always emit a PDB, Release included: a crash is only symbolized right with a matching one.
    add_compile_options(/W4 /permissive- /Zi)
    add_link_options(/DEBUG)
endif()

# Third-party code the reference links as is (zlib 1.2.11, "inflate 1.2.11 Copyright" in the binary).
add_subdirectory(vendor/zlib-1.2.11)

add_subdirectory(src)

# tests: console programs, not part of the client
add_subdirectory(test)

cmake_minimum_required(VERSION 3.20)

if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
    message(FATAL_ERROR "In-source builds are not allowed; configure into build/.")
endif()

# The version is MAJOR.MINOR.PATCH, with the milestones that move it in README.md.
project(frostlite VERSION 0.1.0 LANGUAGES C CXX)

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
    # strcpy, strcat, _strlwr, localtime...: the reference calls these, and their _s forms behave
    # differently (they end the process on overflow). C4996 is the CRT's opinion, not a defect.
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    # Always emit a PDB, Release included: a crash is only symbolized right with a matching one.
    add_compile_options(/W4 /permissive- /Zi)
    add_link_options(/DEBUG)
endif()

# A target's own warning level in place of the project's /W4 (none given: the target's sources
# set their own, with COMPILE_OPTIONS). Added beside /W4, both reached the command line and cl
# warned D9025, "overriding '/W4' with '/W1'", once per file: thousands of lines a translator
# build. Removing it from the target works the same for Ninja (CLion) and Visual Studio.
function(frost_warning_level target)
    if(MSVC)
        get_target_property(options ${target} COMPILE_OPTIONS)
        if(options)
            list(REMOVE_ITEM options /W4)
            set_target_properties(${target} PROPERTIES COMPILE_OPTIONS "${options}")
        endif()
        if(ARGC GREATER 1)
            target_compile_options(${target} PRIVATE ${ARGV1})
        endif()
    endif()
endfunction()

# Third-party code the reference links as is (zlib 1.2.11, "inflate 1.2.11 Copyright" in the binary).
add_subdirectory(vendor/zlib-1.2.11)
# Lua 5.1.4 ("Lua 5.1.4" in the binary), with Blizzard's changes (taint) ported on top from the binary.
add_subdirectory(vendor/lua-5.1.4)
add_subdirectory(vendor/expat-1.95.5)
# FreeType 2.9.1 (FT_New_Library stores version 2.9.1 at 0x141f7be90), the default modules.
add_subdirectory(vendor/freetype-2.9.1)
# FMOD Ex 4.44.64 ("fmod/fmod4/src/fmod.cpp", FSB5/CELT codecs): linked as a DLL.
add_subdirectory(vendor/fmodex-4.44.64)
# libvpx 1.10.0 ("WebM Project VP9 Decoder v1.10.0"): the cinematics are VP9 in AVI.
add_subdirectory(vendor/libvpx-1.10.0)

add_subdirectory(src)

# tests: console programs, not part of the client
add_subdirectory(test)

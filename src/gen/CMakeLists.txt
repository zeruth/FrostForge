# The reference functions tools/translate.py translates from the decompilation (C), the runtime
# they run against (RefImage.cpp), and the C++ forwarders the port registers them through.
file(GLOB GEN_TRANSLATED CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/translated_*.c)
add_library(gen STATIC
    RefImage.cpp
    handlers.cpp
    adapters.c
    stubs.c
    jumps.c
    hits.c
    ${GEN_TRANSLATED}
)
target_include_directories(gen PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gen PRIVATE util lua)
if(MSVC)
    # decompiled C: its implicit conversions are expected; keep the build output for real errors.
    # The level is each source's own (the target's /W4 removed): /W1 for the C, the project's /W4
    # for the C++ (RefImage.cpp, handlers.cpp). A $<COMPILE_LANGUAGE:C> target option was not
    # applied per file by the Visual Studio generator, and with Ninja it landed after /W4, D9025
    # on every file. 4311/4302, pointer truncation, are not disabled: tools/translate.py writes
    # those casts out, and one left is worth seeing
    frost_warning_level(gen)
    set_source_files_properties(adapters.c stubs.c jumps.c hits.c ${GEN_TRANSLATED}
        PROPERTIES COMPILE_OPTIONS "/W1;/wd4047;/wd4024;/wd4133;/wd4312")
    set_source_files_properties(RefImage.cpp handlers.cpp PROPERTIES COMPILE_OPTIONS "/W4")
    # jumps.c holds an entry per runnable function: past COFF's 2^16 section limit
    target_compile_options(gen PRIVATE /bigobj)
    # MSBuild builds one project's sources one after another unless the compiler fans them out:
    # without this the ~140 translated files of every tools/translate.py --fix round compiled
    # serially
    target_compile_options(gen PRIVATE /MP)
    # C17 for _Generic (GEN_FABS and friends in Ghidra.h decide by an operand's type whether it is
    # a float or its bits). Not C23: there `T f();` declares a function of no arguments, and every
    # translated call goes through unprototyped declarations
    target_compile_options(gen PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c17>)
endif()

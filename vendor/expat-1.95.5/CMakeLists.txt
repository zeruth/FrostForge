add_library(expat STATIC
    lib/xmlparse.c
    lib/xmlrole.c
    lib/xmltok.c
)
target_include_directories(expat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_compile_definitions(expat PUBLIC XML_STATIC PRIVATE COMPILED_FROM_DSP)
if(MSVC)
    target_compile_options(expat PRIVATE /W0)
endif()

add_library(expat STATIC
    lib/xmlparse.c
    lib/xmlrole.c
    lib/xmltok.c
)
target_include_directories(expat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
# 1.95.5 spells the static-library switch _STATIC (XML_STATIC is the later name)
target_compile_definitions(expat PUBLIC XML_STATIC _STATIC PRIVATE COMPILED_FROM_DSP)
if(MSVC)
    frost_warning_level(expat /W0)
endif()

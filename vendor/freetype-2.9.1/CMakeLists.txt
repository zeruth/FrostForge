# FreeType 2.9.1 as the reference links it: the default module set (include/freetype/config/
# ftmodule.h) and the bundled zlib 1.1.4 behind the gzip module. Not upstream's build; the
# file list is docs/INSTALL.ANY's.
add_library(freetype STATIC
    src/base/ftsystem.c
    src/base/ftinit.c
    src/base/ftdebug.c
    src/base/ftbase.c
    src/base/ftbbox.c
    src/base/ftbdf.c
    src/base/ftbitmap.c
    src/base/ftcid.c
    src/base/ftfstype.c
    src/base/ftgasp.c
    src/base/ftglyph.c
    src/base/ftgxval.c
    src/base/ftmm.c
    src/base/ftotval.c
    src/base/ftpatent.c
    src/base/ftpfr.c
    src/base/ftstroke.c
    src/base/ftsynth.c
    src/base/fttype1.c
    src/base/ftwinfnt.c
    src/autofit/autofit.c
    src/bdf/bdf.c
    src/cff/cff.c
    src/cid/type1cid.c
    src/gzip/ftgzip.c
    src/lzw/ftlzw.c
    src/pcf/pcf.c
    src/pfr/pfr.c
    src/psaux/psaux.c
    src/pshinter/pshinter.c
    src/psnames/psnames.c
    src/raster/raster.c
    src/sfnt/sfnt.c
    src/smooth/smooth.c
    src/truetype/truetype.c
    src/type1/type1.c
    src/type42/type42.c
    src/winfonts/winfnt.c
)
target_include_directories(freetype PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(freetype PRIVATE FT2_BUILD_LIBRARY _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    # Upstream code, built as is.
    target_compile_options(freetype PRIVATE /W0)
endif()

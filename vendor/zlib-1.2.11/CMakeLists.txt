add_library(zlib STATIC
    adler32.c
    compress.c
    crc32.c
    deflate.c
    infback.c
    inffast.c
    inflate.c
    inftrees.c
    trees.c
    uncompr.c
    zutil.c
)
target_include_directories(zlib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    # Upstream code, built as is.
    frost_warning_level(zlib /W0)
endif()

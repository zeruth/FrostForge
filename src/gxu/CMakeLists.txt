add_library(gxu STATIC
    GxuFontBatch.cpp
    GxuFontFace.cpp
    GxuFontFont.cpp
    GxuFontMain.cpp
    GxuFontMiscClasses.cpp
    GxuFontSlug.cpp
    GxuFontString.cpp
    GxuFontUtil.cpp
    IGxuFontGlyph.cpp
    TextBlock.cpp
)
target_include_directories(gxu PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gxu PUBLIC base blz cas console freetype gx storm util)

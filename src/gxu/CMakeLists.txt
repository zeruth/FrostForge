add_library(gxu STATIC
    GxuFontMain.cpp
)
target_include_directories(gxu PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gxu PUBLIC storm)

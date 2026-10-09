add_library(images STATIC
    Blp.cpp
    ImageFuncs.cpp
    PixelConvert.cpp
)
target_include_directories(images PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(images PUBLIC storm util)

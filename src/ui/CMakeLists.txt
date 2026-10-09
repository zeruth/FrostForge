add_library(ui STATIC
    FrameXML.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC console framescript storm util)

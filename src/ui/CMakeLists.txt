add_library(ui STATIC
    BinaryXML.cpp
    FrameXML.cpp
    UITypes.cpp
    XMLTree.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC base blz console expat framescript gxu storm util)

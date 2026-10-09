add_library(ui STATIC
    BinaryXML.cpp
    CLayoutFrame.cpp
    CSimpleFont.cpp
    FrameXML.cpp
    UITypes.cpp
    XMLTree.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC base blz cas console expat framescript gxu os storm util)

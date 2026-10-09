add_library(ui STATIC
    FrameXML.cpp
    XMLTree.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC base blz console expat framescript storm util)

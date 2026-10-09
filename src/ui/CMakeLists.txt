add_library(ui STATIC
    BinaryXML.cpp
    CLayoutFrame.cpp
    CScriptObject.cpp
    CScriptRegion.cpp
    CSimpleFrame.cpp
    CSimpleTop.cpp
    CSimpleFont.cpp
    FrameXML.cpp
    UITypes.cpp
    XMLTree.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/FrameScriptEvents.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/FrameScriptObject.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC base blz cas console expat framescript glue gxu os storm util)

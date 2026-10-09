add_library(ui STATIC
    BinaryXML.cpp
    CLayoutFrame.cpp
    CAnimatableObject.cpp
    CRenderBatch.cpp
    CScriptObject.cpp
    CScriptObjectScript.cpp
    CScriptRegion.cpp
    CScriptRegionScript.cpp
    CSimpleFrame.cpp
    CSimpleFrameScript.cpp
    CSimpleRegion.cpp
    CSimpleTop.cpp
    CSimpleFont.cpp
    FrameXML.cpp
    UITypes.cpp
    XMLTree.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/FrameScriptEvents.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/FrameScriptObject.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/ScriptMethod.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC base blz cas console expat framescript glue gxu os storm util)

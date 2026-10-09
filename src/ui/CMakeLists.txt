add_library(ui STATIC
    BinaryXML.cpp
    CLayoutFrame.cpp
    CAnimatableObject.cpp
    CRenderBatch.cpp
    CScriptObject.cpp
    CScriptObjectScript.cpp
    CScriptRegion.cpp
    CScriptRegionScript.cpp
    CSimpleButton.cpp
    CSimpleButtonScript.cpp
    CSimpleCheckButton.cpp
    CSimpleScrollFrame.cpp
    CSimpleSlider.cpp
    CSimpleFrame.cpp
    CSimpleFrameScript.cpp
    CSimpleRegion.cpp
    CSimpleTexture.cpp
    CSimpleTextureScript.cpp
    CSimpleTop.cpp
    TextureAtlas.cpp
    CSimpleFont.cpp
    CSimpleFontScript.cpp
    CSimpleFontString.cpp
    CSimpleFontStringScript.cpp
    FrameScriptGlobals.cpp
    FrameXML.cpp
    UITypes.cpp
    XMLTree.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/FrameScriptEvents.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/FrameScriptObject.cpp
    ${CMAKE_SOURCE_DIR}/src/framescript/ScriptMethod.cpp
)
target_include_directories(ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ui PUBLIC base blz cas client console db expat framescript glue gx gxu os shadereffect storm texture util)

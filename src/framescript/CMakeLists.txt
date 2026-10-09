add_library(framescript STATIC
    LuaHost.cpp
    LuaMemPool.cpp
    ScriptAPI.cpp
    FrameScript.cpp
    FrameScriptLibrary.cpp
)
target_include_directories(framescript PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(framescript PUBLIC base blz cas lua os storm util)

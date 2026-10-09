add_library(framescript STATIC
    LuaHost.cpp
    LuaMemPool.cpp
    FrameScript.cpp
    FrameScriptLibrary.cpp
    FrameScriptObject.cpp
)
target_include_directories(framescript PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(framescript PUBLIC base blz lua os storm util)

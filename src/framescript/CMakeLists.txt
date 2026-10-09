add_library(framescript STATIC
    LuaHost.cpp
    LuaMemPool.cpp
)
target_include_directories(framescript PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(framescript PUBLIC lua os storm util)

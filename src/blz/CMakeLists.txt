add_library(blz STATIC
    AllocString.cpp
    Allocator.cpp
    Function.cpp
    Hex.cpp
    Log.cpp
    Md5.cpp
    Murmur.cpp
    ObjectPool.cpp
    Parse.cpp
    String.cpp
    Sync.cpp
)
target_include_directories(blz PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(blz PUBLIC storm)

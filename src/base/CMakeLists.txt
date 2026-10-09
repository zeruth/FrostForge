add_library(base STATIC
    CharClass.cpp
    Handle.cpp
    Matrix.cpp
    Coordinate.cpp
    ObjectAlloc.cpp
    DataAllocator.cpp
    RCString.cpp
    Status.cpp
    StringAllocator.cpp
)
target_include_directories(base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(base PUBLIC blz os storm)

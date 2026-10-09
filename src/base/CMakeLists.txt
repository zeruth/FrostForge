add_library(base STATIC
    RCString.cpp
    Status.cpp
)
target_include_directories(base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(base PUBLIC blz storm)

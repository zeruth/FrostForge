add_library(aio STATIC
    Aio.cpp
)
target_include_directories(aio PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(aio PUBLIC blz os storm util)

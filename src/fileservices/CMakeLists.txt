add_library(fileservices STATIC
    AsyncFile.cpp
)
target_include_directories(fileservices PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(fileservices PUBLIC base blz cas gx os storm util)

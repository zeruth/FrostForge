add_library(console STATIC
    Command.cpp
    Var.cpp
)
target_include_directories(console PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(console PUBLIC base blz gx os storm util)

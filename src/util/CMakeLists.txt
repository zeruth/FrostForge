add_library(util STATIC
    FunctionPointer.cpp
    Fnv.cpp
    Unimplemented.cpp
)
target_include_directories(util PUBLIC ${CMAKE_SOURCE_DIR}/src)
# CheckFunctionPointer reports through storm, which links util: a static-library cycle
target_link_libraries(util PUBLIC storm)

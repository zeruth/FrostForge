add_library(model2 STATIC
    CM2Lighting.cpp
    M2Api.cpp
)
target_include_directories(model2 PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(model2 PUBLIC base util)

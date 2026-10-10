add_library(model2 STATIC
    CM2Lighting.cpp
    M2Api.cpp
    M2Cache.cpp
    M2Init.cpp
    M2Model.cpp
    M2Shaders.cpp
    M2Shared.cpp
)
target_include_directories(model2 PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(model2 PUBLIC base util cas console fileservices gx shadereffect storm texture)

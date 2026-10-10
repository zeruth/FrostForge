add_library(shadereffect STATIC
    PipelineMap.cpp
    ShaderEffectManager.cpp
    ShaderEffectPipeline.cpp
)
target_include_directories(shadereffect PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(shadereffect PUBLIC blz client gx os storm util)

add_library(shadereffect STATIC
    PipelineMap.cpp
    ShaderEffectContext.cpp
    ShaderEffectManager.cpp
    ShaderEffectPipeline.cpp
)
target_include_directories(shadereffect PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(shadereffect PUBLIC base blz client gx os storm texture util)

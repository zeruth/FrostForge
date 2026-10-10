add_library(model2 STATIC
    BaseModel.cpp
    CM2Lighting.cpp
    M2Api.cpp
    M2Cache.cpp
    M2Camera.cpp
    M2Element.cpp
    M2Init.cpp
    M2Model.cpp
    M2ModelAnimate.cpp
    M2ModelAttach.cpp
    M2ModelUpdate.cpp
    M2ModelApi.cpp
    M2ModelEvents.cpp
    M2ModelInit.cpp
    M2ModelVirtual.cpp
    M2Particle.cpp
    M2ParticleGenerator.cpp
    M2ParticleRender.cpp
    M2Ribbon.cpp
    M2Scene.cpp
    M2SceneAnimate.cpp
    M2SceneBuild.cpp
    M2SceneDraw.cpp
    M2SceneSort.cpp
    M2Sequence.cpp
    M2Shaders.cpp
    M2Shared.cpp
    M2Skeleton.cpp
    M2Track.cpp
)
target_include_directories(model2 PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(model2 PUBLIC base util cas console fileservices gx shadereffect storm texture)

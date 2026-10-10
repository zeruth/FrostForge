add_library(world STATIC
    CullWorker.cpp
    ModelFog.cpp
    ParticulateVolumeManager.cpp
    SceneOcclusion.cpp
    WorldClip.cpp
    WorldFade.cpp
    WorldFrame.cpp
    WorldSceneDraw.cpp
    WorldMap.cpp
    WorldScene.cpp
    WorldSceneCallbacks.cpp
    WorldSceneRender.cpp
    WorldStubs.cpp
    WorldView.cpp
)
target_include_directories(world PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(world PUBLIC base blz client daynight gx model2 os shadereffect storm threadpool util)

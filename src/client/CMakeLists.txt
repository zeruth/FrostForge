add_library(client STATIC
    ClientCVars.cpp
    GraphicsSettings.cpp
    RenderCVars.cpp
    ClientLoop.cpp
    CommandLine.cpp
    ConsoleDevice.cpp
    CrashReport.cpp
    GxWindow.cpp
    LoadingBar.cpp
    Locale.cpp
    LocalFileHandlers.cpp
    Main.cpp
    Startup.cpp
    FrameBufferCache.cpp
    Resample.cpp
    RenderSettings.cpp
    StartupClient.cpp
    StartupService.cpp
)
target_include_directories(client PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(client PUBLIC base cas console db fileservices framescript glue gx os services shadereffect sound storm texture threadpool ui util)

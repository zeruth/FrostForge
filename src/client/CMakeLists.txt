add_library(client STATIC
    ClientLoop.cpp
    CommandLine.cpp
    ConsoleDevice.cpp
    CrashReport.cpp
    GxWindow.cpp
    Locale.cpp
    LocalFileHandlers.cpp
    Main.cpp
    Startup.cpp
    FrameBufferCache.cpp
    RenderSettings.cpp
    StartupClient.cpp
    StartupService.cpp
)
target_include_directories(client PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(client PUBLIC base cas console gx os services storm threadpool util)

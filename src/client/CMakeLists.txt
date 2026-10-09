add_library(client STATIC
    CommandLine.cpp
    CrashReport.cpp
    Locale.cpp
    LocalFileHandlers.cpp
    Main.cpp
    Startup.cpp
    StartupService.cpp
)
target_include_directories(client PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(client PUBLIC base cas console os services storm util)

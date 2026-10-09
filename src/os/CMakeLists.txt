add_library(os STATIC
    win/File.cpp
    win/FileOps.cpp
    win/Timer.cpp
    win/Cursor.cpp
    win/Window.cpp
    win/Path.cpp
    win/PathTree.cpp
    win/SharedMemory.cpp
    win/SockAddr.cpp
    win/System.cpp
    win/Sync.cpp
    win/Thread.cpp
    win/ThreadEntry.cpp
)
target_include_directories(os PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(os PUBLIC storm util winmm advapi32)

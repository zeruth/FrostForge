add_executable(FrostLite WIN32
    win/WinMain.cpp
)
target_link_libraries(FrostLite PRIVATE client)

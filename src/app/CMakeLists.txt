add_executable(FrostLite WIN32
    win/WinMain.cpp
)
target_link_libraries(FrostLite PRIVATE client)
# FMOD Ex runs from a DLL beside the executable.
add_custom_command(TARGET FrostLite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FMODEX_DLL} $<TARGET_FILE_DIR:FrostLite>
)

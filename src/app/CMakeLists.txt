add_executable(FrostLite WIN32
    win/WinMain.cpp
    win/FrostLite.manifest
)
target_link_libraries(FrostLite PRIVATE client gen)
# The reference image is mapped at its own base (0x140000000) for the translated functions
# (src/gen/RefImage.cpp), so the port lives elsewhere.
# Fixed, so ASLR cannot place it over that range.
target_link_options(FrostLite PRIVATE /BASE:0x180000000 /DYNAMICBASE:NO)
# FMOD Ex runs from a DLL beside the executable.
add_custom_command(TARGET FrostLite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FMODEX_DLL} $<TARGET_FILE_DIR:FrostLite>
)

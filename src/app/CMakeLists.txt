add_executable(FrostLite WIN32
    win/WinMain.cpp
    win/FrostLite.manifest
)
target_link_libraries(FrostLite PRIVATE client gen)
# The reference image is mapped at its own base (0x140000000) for the translated functions
# (src/gen/RefImage.cpp), so the port lives elsewhere.
# Fixed, so ASLR cannot place it over that range.
target_link_options(FrostLite PRIVATE /BASE:0x180000000 /DYNAMICBASE:NO)
# A deliberate divergence: the reference reserves 1 MB for the main thread, but a translated
# function's frame is several times the original's (the zeroed gen_frame block, the locals beside
# it, the parameter slots), so a chain that fits in 1 MB there does not here. The character
# select model's sequence callbacks (M2ResolveSequence's 7 KB array among them) overflowed it,
# 2026-10-21.
target_link_options(FrostLite PRIVATE /STACK:0x1000000)
# Hand ports whose only callers are translated: nothing in the port names them, so their object
# would stay out of the link and the translator could never tie them (src/client/DBCache.cpp).
target_link_options(FrostLite PRIVATE
    "/INCLUDE:?DBCacheGetBuild@@YAIXZ"
    "/INCLUDE:?DBCacheGetDirectory@@YAXPEAD_K@Z"
)
# FMOD Ex runs from a DLL beside the executable.
add_custom_command(TARGET FrostLite POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FMODEX_DLL} $<TARGET_FILE_DIR:FrostLite>
)

# FMOD Ex 4.44.64 (Firelight Technologies, licence in LICENSE.TXT). The reference links FMOD Ex
# 4.44 statically (Engine/Source/SoundEngine/fmod/fmod4: FSB5 and CELT codecs, WASAPI output,
# the 4-argument getDriverCaps); the sound engine is ported over the same API through the DLL.
add_library(fmodex SHARED IMPORTED GLOBAL)
set_target_properties(fmodex PROPERTIES
    IMPORTED_IMPLIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/fmodex64_vc.lib
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/lib/fmodex64.dll
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
set(FMODEX_DLL ${CMAKE_CURRENT_SOURCE_DIR}/lib/fmodex64.dll CACHE INTERNAL "")

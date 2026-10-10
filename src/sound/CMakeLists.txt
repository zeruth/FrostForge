add_library(sound STATIC
    SE3.cpp
    SE3Bus.cpp
    SE3Cache.cpp
    SE3Emitter.cpp
    SE3Instance.cpp
    SE3Play.cpp
    SE3PlayKit.cpp
    SE3SoundKit.cpp
    SE3Start.cpp
    SE3Suppressors.cpp
    SE3Update.cpp
    SI3.cpp
    SI3CVars.cpp
    SI3Sound.cpp
)
target_include_directories(sound PUBLIC ${CMAKE_SOURCE_DIR}/src)
# SI3 reads the command line and sets the main loop's pending reset in client, which links
# sound back: a static-library cycle, as with services
target_link_libraries(sound PUBLIC base blz client console db fmodex framescript os services storm ui util)

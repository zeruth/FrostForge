add_library(sound STATIC
    SE3.cpp
    SE3Bus.cpp
    SE3SoundKit.cpp
    SI3.cpp
    SI3CVars.cpp
)
target_include_directories(sound PUBLIC ${CMAKE_SOURCE_DIR}/src)
# SI3 reads the command line and sets the main loop's pending reset in client, which links
# sound back: a static-library cycle, as with services
target_link_libraries(sound PUBLIC base blz client console db fmodex framescript os services storm ui util)

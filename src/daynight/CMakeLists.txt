add_library(daynight STATIC
    DayNight.cpp
    DNBodies.cpp
    DNClouds.cpp
    DNLight.cpp
    DNSky.cpp
)
target_include_directories(daynight PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(daynight PUBLIC base blz client db gx model2 storm texture ui util world)

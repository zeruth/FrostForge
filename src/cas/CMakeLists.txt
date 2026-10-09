add_library(cas STATIC
    CAS.cpp
    CASLocalManifest.cpp
    CASManifest.cpp
)
target_include_directories(cas PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(cas PUBLIC base blz casc os storm tact util)

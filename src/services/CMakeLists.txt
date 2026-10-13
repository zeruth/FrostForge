add_library(services STATIC
    Agent.cpp
    BattleNet.cpp
    ClientServices.cpp
    Telemetry.cpp
)
target_include_directories(services PUBLIC ${CMAKE_SOURCE_DIR}/src)
# ClientServices reads the locale/region state in client, which links services back: a
# static-library cycle, as with storm and os
target_link_libraries(services PUBLIC base blz client console fileservices os storm ui util)

add_library(net STATIC
    NetClient.cpp
    NetInternal.cpp
    RealmConnection.cpp
    WowConnection.cpp
)
target_include_directories(net PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(net PUBLIC base blz console os storm util zlib)

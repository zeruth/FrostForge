add_library(db STATIC
    DB2Tables.cpp
    DBClient.cpp
    DynamicLifeTimeDB.cpp
    TactKeys.cpp
    WowClientSparseDB2.cpp
    WowClientDB2.cpp
    WowDB2Lookup.cpp
)
target_include_directories(db PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(db PUBLIC base blz cas fileservices storm util)
# DB2Tables.cpp holds every table's meta: one large translation unit.
if(MSVC)
    target_compile_options(db PRIVATE /bigobj)
endif()

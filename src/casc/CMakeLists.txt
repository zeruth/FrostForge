add_library(casc STATIC
    ArchiveFileAccessor.cpp
    ArchiveFileCache.cpp
    Common.cpp
    ContainerIndex.cpp
    Dynamic.cpp
    DynamicStorage.cpp
    Hash.cpp
    IndexTables.cpp
    KeyMappingTable.cpp
    SegmentHeader.cpp
    Shmem.cpp
)
target_include_directories(casc PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(casc PUBLIC aio blz os storm util advapi32)

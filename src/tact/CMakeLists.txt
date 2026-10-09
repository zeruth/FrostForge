add_library(tact STATIC
    AsyncState.cpp
    BlockCache.cpp
    Codec.cpp
    Common.cpp
    ConfigRead.cpp
    ConfigReader.cpp
    Container.cpp
    DownloadQueue.cpp
    DynamicInit.cpp
    EncodingTable.cpp
    FileManifest.cpp
    ImmediateRead.cpp
    FileHandle.cpp
    HeaderCache.cpp
    SpanResident.cpp
    FileRead.cpp
    Instance.cpp
    InstallationInfo.cpp
    Manifest.cpp
    PatchIndex.cpp
    Psv.cpp
    ReadQueue.cpp
    TaskScheduler.cpp
    U32Set.cpp
)
target_include_directories(tact PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(tact PUBLIC aio blz casc os storm util zlib)

add_executable(PrintfTest storm/PrintfTest.cpp)
target_link_libraries(PrintfTest PRIVATE storm)

add_executable(CasExtract tools/CasExtract.cpp)
target_link_libraries(CasExtract PRIVATE base blz cas client db storm)

add_executable(DB2Check tools/DB2Check.cpp)
target_link_libraries(DB2Check PRIVATE base blz cas client db storm)

add_executable(ProbeTest storm/ProbeTest.cpp)
target_link_libraries(ProbeTest PRIVATE base cas client console gx gxu os prism storm tact)

add_executable(BinaryXMLTest ui/BinaryXMLTest.cpp)
target_link_libraries(BinaryXMLTest PRIVATE base ui)

add_executable(ObjectAllocTest base/ObjectAllocTest.cpp)
target_link_libraries(ObjectAllocTest PRIVATE base)

add_executable(FrameScriptEventsTest ui/FrameScriptEventsTest.cpp)
target_link_libraries(FrameScriptEventsTest PRIVATE base ui)

add_executable(LayoutFrameTest ui/LayoutFrameTest.cpp)
target_link_libraries(LayoutFrameTest PRIVATE base ui)

add_executable(SimpleFrameTest ui/SimpleFrameTest.cpp)
target_link_libraries(SimpleFrameTest PRIVATE base ui)

add_executable(ImagesTest images/ImagesTest.cpp)
target_link_libraries(ImagesTest PRIVATE images)

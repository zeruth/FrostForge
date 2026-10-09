add_executable(PrintfTest storm/PrintfTest.cpp)
target_link_libraries(PrintfTest PRIVATE storm)

add_executable(ProbeTest storm/ProbeTest.cpp)
target_link_libraries(ProbeTest PRIVATE base cas client console gx os prism storm tact)

add_executable(BinaryXMLTest ui/BinaryXMLTest.cpp)
target_link_libraries(BinaryXMLTest PRIVATE base ui)

add_executable(ObjectAllocTest base/ObjectAllocTest.cpp)
target_link_libraries(ObjectAllocTest PRIVATE base)

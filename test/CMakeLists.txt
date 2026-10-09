add_executable(PrintfTest storm/PrintfTest.cpp)
target_link_libraries(PrintfTest PRIVATE storm)

add_executable(ProbeTest storm/ProbeTest.cpp)
target_link_libraries(ProbeTest PRIVATE base cas client os storm tact)

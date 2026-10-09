add_library(gx STATIC
    GxApi.cpp
    GxLog.cpp
    win/GxDeviceW32.cpp
)
target_include_directories(gx PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gx PUBLIC blz client console os prism storm util dxguid)

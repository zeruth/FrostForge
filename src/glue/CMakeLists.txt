add_library(glue STATIC
    GlueMgr.cpp
)
target_include_directories(glue PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(glue PUBLIC client framescript os storm ui util)

add_library(threadpool STATIC
    ThreadPool.cpp
)
target_include_directories(threadpool PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(threadpool PUBLIC blz os storm util)

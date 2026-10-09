add_library(texture STATIC
    Texture.cpp
)
target_include_directories(texture PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(texture PUBLIC base blz cas fileservices gx images os storm threadpool util)

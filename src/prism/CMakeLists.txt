add_library(prism STATIC
    Dx11.cpp
    Dx12.cpp
    Dxgi.cpp
    Instance.cpp
    Intel.cpp
)
target_include_directories(prism PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(prism PUBLIC blz storm util dxguid setupapi)

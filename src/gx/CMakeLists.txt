add_library(gx STATIC
    GxApi.cpp
    GxBuf.cpp
    GxFormat.cpp
    GxPipeline.cpp
    GxVertexFormat.cpp
    GxDevice.cpp
    GxLog.cpp
    GxRenderContext.cpp
    GxShader.cpp
    d3d12/Binding.cpp
    d3d12/CommandList.cpp
    d3d12/Draw.cpp
    d3d12/Device.cpp
    d3d12/Frame.cpp
    d3d12/Pipeline.cpp
    d3d12/Residency.cpp
    d3d12/Samplers.cpp
    d3d12/Upload.cpp
    d3d12/Tags.cpp
    d3d12/Tex.cpp
    win/GxDeviceW32.cpp
)
target_include_directories(gx PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(gx PUBLIC blz cas client images console os prism storm threadpool util zlib d3d12 dxgi dxguid)

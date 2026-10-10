add_library(glue STATIC
    GlueMgr.cpp
    GlobalColor.cpp
    GlueScriptAPI.cpp
    ScriptAPICVar.cpp
    ScriptAPIGlue.cpp
    EventSignals.cpp
    ScriptAPIRegistry.cpp
    ScriptAPIStubs.cpp
)
target_include_directories(glue PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(glue PUBLIC client framescript os storm ui util)

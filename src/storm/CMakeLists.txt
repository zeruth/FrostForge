add_library(storm STATIC
    Command.cpp
    ErrorReport.cpp
    Log.cpp
    Memory.cpp
    Printf.cpp
    String.cpp
    Unicode.cpp
    thread/SCritSect.cpp
    thread/SThread.cpp
)
target_include_directories(storm PUBLIC ${CMAKE_SOURCE_DIR}/src)
# storm and os call each other (SCmd reads @response files through OsFile; Os paths use
# storm's Unicode and error-report state), as in the reference: a static-library cycle
target_link_libraries(storm PUBLIC blz os util)

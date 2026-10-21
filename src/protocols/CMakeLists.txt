# Multi-protocol transport channels (SSH2 / serial / telnet / local PTY).
# All channels implement ITransportChannel; the interface is frozen after P1-T9.

add_library(openscrt_protocols STATIC
    ModuleInfo.cpp
    ITransportChannel.cpp)

target_include_directories(openscrt_protocols PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_protocols PUBLIC openscrt_warnings PRIVATE openscrt_core)

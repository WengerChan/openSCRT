# Embedded Python host and the SecureCRT-compatible `crt` object model (Phase 5).

add_library(openscrt_scripting STATIC ModuleInfo.cpp)

target_include_directories(openscrt_scripting PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_scripting PUBLIC openscrt_warnings PRIVATE openscrt_core)

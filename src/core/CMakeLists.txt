# Core data structures: lock-free transport queues and locked credential buffers.
# Depends on nothing inside the project so it stays unit-testable in isolation.

add_library(openscrt_core STATIC
    ModuleInfo.cpp
    SecureByteArray.cpp)

target_include_directories(openscrt_core PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_compile_definitions(openscrt_core PUBLIC OPENCRT_VERSION="${PROJECT_VERSION}")

target_link_libraries(openscrt_core
    PUBLIC openscrt_warnings
    PRIVATE openscrt_sodium)

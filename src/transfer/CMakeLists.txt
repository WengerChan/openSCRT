# File transfer: SFTP subsystem plus inline X/Y/Zmodem state machines.

add_library(openscrt_transfer STATIC ModuleInfo.cpp)

target_include_directories(openscrt_transfer PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_transfer PUBLIC openscrt_warnings PRIVATE openscrt_core openscrt_terminal)

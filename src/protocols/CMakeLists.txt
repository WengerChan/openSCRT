# Multi-protocol transport channels (SSH2 / serial / telnet / local PTY).
# All channels implement ITransportChannel; the interface is frozen after P1-T9.

# libssh (P1-T11). Found by path rather than by package config: the vcpkg port ships the
# headers and a pkg-config file, and find_path/find_library work against the installed
# tree regardless of which metadata the port happens to provide.
find_path(LIBSSH_INCLUDE_DIR NAMES libssh/libssh.h)
find_library(LIBSSH_LIBRARY NAMES ssh libssh)
if(NOT LIBSSH_INCLUDE_DIR OR NOT LIBSSH_LIBRARY)
    message(FATAL_ERROR
        "openscrt_protocols needs libssh (P1-T11). It is declared in vcpkg.json; if CI "
        "skipped it, delete its entry in the SKIP table in .github/scripts/ci-vcpkg-manifest.py.")
endif()

add_library(openscrt_ssh INTERFACE)
target_include_directories(openscrt_ssh SYSTEM INTERFACE ${LIBSSH_INCLUDE_DIR})
target_link_libraries(openscrt_ssh INTERFACE ${LIBSSH_LIBRARY})

add_library(openscrt_protocols STATIC
    ModuleInfo.cpp
    ITransportChannel.cpp
    local/LocalPtyChannel.cpp
    ssh/Ssh2Channel.cpp)

target_include_directories(openscrt_protocols PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_protocols
    PUBLIC openscrt_warnings
    PRIVATE openscrt_core openscrt_terminal openscrt_ssh)

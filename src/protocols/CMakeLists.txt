# Multi-protocol transport channels (SSH2 / serial / telnet / local PTY).
# All channels implement ITransportChannel; the interface is frozen after P1-T9.

# libssh (P1-T11).
#
# Preferred path: the vcpkg port installs a CMake package config, and that config does
# find_dependency(OpenSSL) for us. libssh is a static library, so whoever links it must
# also link the OpenSSL it was built against -- skipping that is what produced
# "undefined reference to BN_new" in run #39894293542.
#
# Fallback path: find the library by path and link the vcpkg tree's OpenSSL explicitly.
# The fallback searches the vcpkg installed tree first so it cannot pick up a system
# OpenSSL of a different ABI than the one libssh was compiled against.
find_package(libssh CONFIG QUIET)

if(TARGET libssh::libssh_static)
    add_library(openscrt_ssh INTERFACE)
    target_link_libraries(openscrt_ssh INTERFACE libssh::libssh_static)
    message(STATUS "libssh: using libssh::libssh_static")
elseif(TARGET libssh::libssh_shared)
    add_library(openscrt_ssh INTERFACE)
    target_link_libraries(openscrt_ssh INTERFACE libssh::libssh_shared)
    message(STATUS "libssh: using libssh::libssh_shared")
else()
    message(STATUS "libssh: no CMake package config, falling back to a path search")
    find_path(LIBSSH_INCLUDE_DIR NAMES libssh/libssh.h)
    find_library(LIBSSH_LIBRARY NAMES ssh libssh)
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(OPENSCRT_VCPKG_LIB_DIR "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    else()
        set(OPENSCRT_VCPKG_LIB_DIR "")
    endif()

    find_library(LIBSSH_SSL_LIBRARY NAMES ssl
                 PATHS "${OPENSCRT_VCPKG_LIB_DIR}" NO_DEFAULT_PATH)
    find_library(LIBSSH_CRYPTO_LIBRARY NAMES crypto
                 PATHS "${OPENSCRT_VCPKG_LIB_DIR}" NO_DEFAULT_PATH)
    if(NOT LIBSSH_SSL_LIBRARY OR NOT LIBSSH_CRYPTO_LIBRARY)
        find_library(LIBSSH_SSL_LIBRARY NAMES ssl)
        find_library(LIBSSH_CRYPTO_LIBRARY NAMES crypto)
    endif()

    if(NOT LIBSSH_INCLUDE_DIR OR NOT LIBSSH_LIBRARY OR NOT LIBSSH_SSL_LIBRARY OR
       NOT LIBSSH_CRYPTO_LIBRARY)
        message(FATAL_ERROR
            "openscrt_protocols needs libssh and its OpenSSL (P1-T11). libssh is declared in "
            "vcpkg.json; if CI skipped it, delete its entry in the SKIP table in "
            ".github/scripts/ci-vcpkg-manifest.py.")
    endif()

    add_library(openscrt_ssh INTERFACE)
    target_include_directories(openscrt_ssh SYSTEM INTERFACE ${LIBSSH_INCLUDE_DIR})
    target_link_libraries(openscrt_ssh INTERFACE
        ${LIBSSH_LIBRARY} ${LIBSSH_SSL_LIBRARY} ${LIBSSH_CRYPTO_LIBRARY})
endif()

# OpenSSL is used directly, not only through libssh: the ssh.com (VanDyke) private key
# format needs 3DES-CBC to decrypt the key body and MD5 to derive the cipher key.
#
# The version that matters is the one in the vcpkg tree -- libssh is a static library
# built against that exact OpenSSL, and mixing it with the system one is how a client
# ends up with "undefined reference to BN_new".
find_package(OpenSSL REQUIRED)

# Qt6::SerialPort for the serial channel (P3-T1). The serial port is the one transport
# that is not a socket, so it needs the one Qt module that is not about sockets.
#
# The dependency is PRIVATE on purpose: the channel's own headers keep Qt out of the
# protocols layer's public surface (see src/protocols/serial/SerialSettings.h), so a
# consumer of ITransportChannel does not need Qt's include directories. The link
# requirement still reaches the executable, because a static library's private
# dependencies are propagated to whoever links it.
if(NOT OPENCRT_QT_SERIALPORT_AVAILABLE)
    message(FATAL_ERROR
        "openscrt_protocols needs Qt6::SerialPort for the serial channel (P3-T1). The CI "
        "Qt step installs the qtserialport module; see docs/DEVELOPMENT_PLAN.md D2.")
endif()

add_library(openscrt_protocols STATIC
    ModuleInfo.cpp
    ITransportChannel.cpp
    local/LocalPtyChannel.cpp
    serial/SerialChannel.cpp
    serial/SerialPortSettings.cpp
    serial/SerialSettings.cpp
    ssh/Ssh2Channel.cpp
    ssh/SshComKey.cpp)

target_include_directories(openscrt_protocols PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_protocols
    PUBLIC openscrt_warnings
    PRIVATE openscrt_core openscrt_terminal openscrt_ssh OpenSSL::Crypto Qt6::SerialPort)

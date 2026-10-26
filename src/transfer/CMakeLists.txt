# File transfer: SFTP subsystem plus inline X/Y/Zmodem state machines.

# The SFTP subsystem (P3-T4) is opened on the SSH session that is already connected,
# so it needs libssh -- the same library the protocols layer uses, and the same
# static one, so it has to be the same OpenSSL as well.
#
# The target itself comes from the protocols layer, which found libssh and worked out
# where its OpenSSL is. Reusing it is what keeps the two halves on one OpenSSL.
if(NOT TARGET openscrt_ssh)
    message(FATAL_ERROR
        "openscrt_transfer needs libssh for the SFTP subsystem (P3-T4). libssh is "
        "declared in vcpkg.json; if CI skipped it, delete its entry in the SKIP table "
        "in .github/scripts/ci-vcpkg-manifest.py.")
endif()

add_library(openscrt_transfer STATIC
    ModuleInfo.cpp
    sftp/SftpSubsystem.cpp)

target_include_directories(openscrt_transfer PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_transfer
    PUBLIC openscrt_warnings
    PRIVATE openscrt_core openscrt_terminal openscrt_ssh)

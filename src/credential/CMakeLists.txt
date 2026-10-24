# KeePass dual-mode credential centre: local .kdbx decryption plus KeePassHTTP RPC.

# OpenSSL for the KDBX cryptography (SHA-256, HMAC-SHA256, AES-256-CBC) and zlib for
# the GZip-compressed payload. Both are declared in vcpkg.json, and the SKIP table in
# .github/scripts/ci-vcpkg-manifest.py no longer skips them -- the contract is that a
# port is un-skipped in the same commit that starts linking it.
#
# OpenSSL is already built as libssh's dependency, so naming it here costs no extra
# build time; zlib is a small build of its own.
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

add_library(openscrt_credential STATIC
    ModuleInfo.cpp
    ICredentialProvider.cpp
    kdbx/KdbxHeaderParser.cpp
    kdbx/KdbxCrypto.cpp
    kdbx/Kdbx4Reader.cpp)

target_include_directories(openscrt_credential PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_credential
    PUBLIC openscrt_warnings
    PRIVATE openscrt_core openscrt_sodium OpenSSL::Crypto ZLIB::ZLIB)

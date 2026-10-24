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

# pugixml for the XML inside a KeePass database (P2-T6). It is no longer skipped in
# .github/scripts/ci-vcpkg-manifest.py.
find_package(pugixml REQUIRED)

# nlohmann-json for the KeePassHTTP payloads (P2-T8) and Qt6::Network for the socket
# the plugin listens on.
find_package(nlohmann_json REQUIRED)
if(NOT Qt6Network_FOUND)
    message(FATAL_ERROR
        "openscrt_credential needs Qt6::Network for the KeePassHTTP transport. Qt comes "
        "from install-qt-action; see docs/DEVELOPMENT_PLAN.md D2.")
endif()

# The vault watches the database file and times out on its own, so it needs QtCore --
# QtCore and not QtWidgets: a file watcher and a timer need no display, and the
# credential layer stays testable headless.
set(CMAKE_AUTOMOC ON)

add_library(openscrt_credential STATIC
    ModuleInfo.cpp
    ICredentialProvider.cpp
    kdbx/KdbxHeaderParser.cpp
    kdbx/KdbxCrypto.cpp
    kdbx/Kdbx3Reader.cpp
    kdbx/Kdbx4Reader.cpp
    kdbx/KdbxProtectedStream.cpp
    kdbx/KdbxVault.cpp
    kdbx/KdbxXmlIndex.cpp
    keepasshttp/KeePassHttpClient.cpp
    keepasshttp/KeePassHttpCrypto.cpp
    keepasshttp/HttpTransport.cpp)

target_include_directories(openscrt_credential PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_credential
    PUBLIC openscrt_warnings Qt6::Core Qt6::Network
    PRIVATE openscrt_core openscrt_sodium OpenSSL::Crypto ZLIB::ZLIB pugixml::pugixml
            nlohmann_json::nlohmann_json)

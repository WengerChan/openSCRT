# KeePass dual-mode credential centre: local .kdbx decryption plus KeePassHTTP RPC.

add_library(openscrt_credential STATIC
    ModuleInfo.cpp
    ICredentialProvider.cpp)

target_include_directories(openscrt_credential PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_credential
    PUBLIC openscrt_warnings
    PRIVATE openscrt_core openscrt_sodium)

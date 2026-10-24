# Global/session option hierarchy, Default Session inheritance, SecureCRT config import.

add_library(openscrt_config STATIC
    ModuleInfo.cpp
    ButtonBarModel.cpp
    ConfigManager.cpp
    SessionProfile.cpp
    importers/SecureCrtConfigImporter.cpp)

target_include_directories(openscrt_config PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_config PUBLIC openscrt_warnings PRIVATE openscrt_core Qt6::Core)

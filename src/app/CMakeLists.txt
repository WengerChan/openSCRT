# Application entry point. `--version` reports every linked module version so a bug report
# can be matched to an exact build.

add_executable(openscrt main.cpp)

target_link_libraries(openscrt PRIVATE openscrt_core openscrt_terminal openscrt_protocols
                                openscrt_transfer openscrt_credential openscrt_scripting
                                openscrt_config openscrt_ui)

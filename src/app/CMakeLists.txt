# Application entry point. `--version` reports every linked module version so a bug report
# can be matched to an exact build.

add_executable(openscrt main.cpp)

target_link_libraries(openscrt PRIVATE openscrt_core openscrt_terminal openscrt_protocols
                                openscrt_transfer openscrt_credential openscrt_scripting
                                openscrt_config openscrt_ui)

# Install layout. `cmake --install <build> --config Release --prefix <staging>` is the single
# place that decides what lands in an install tree; the release workflow stages with it and then
# runs windeployqt over the result, so the portable ZIP and the MSI contain byte-identical files.
# A CLI stub has no icon or desktop entry to install yet -- those arrive with the Qt shell (P1-T12).
install(TARGETS openscrt RUNTIME DESTINATION bin)

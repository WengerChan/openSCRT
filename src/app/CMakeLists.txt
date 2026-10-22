# Application entry point. `--version` reports every linked module version so a bug report
# can be matched to an exact build.

add_executable(openscrt main.cpp)

target_link_libraries(openscrt PRIVATE openscrt_core openscrt_terminal openscrt_protocols
                                openscrt_transfer openscrt_credential openscrt_scripting
                                openscrt_config openscrt_ui)

# MessageBoxA, for the one case a console build cannot cover by itself: launched by double-click,
# where the console window would flash and close before the user could read it.
if(WIN32)
    target_link_libraries(openscrt PRIVATE user32)
endif()

# Install layout. `cmake --install <build> --config Release --prefix <staging>` is the single
# place that decides what lands in an install tree; the release workflow stages with it and then
# runs windeployqt over the result, so the portable ZIP and the MSI contain byte-identical files.
# A CLI stub has no icon or desktop entry to install yet -- those arrive with the Qt shell (P1-T12).
install(TARGETS openscrt RUNTIME DESTINATION bin)

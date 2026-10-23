# Application entry point. `--version` reports every linked module version so a bug report
# can be matched to an exact build.

add_executable(openscrt main.cpp)

# The workspace shell is a Qt Widgets application, so the executable itself has to link the
# widget stack -- a static library's private Qt link does not reach the final binary.
target_link_libraries(openscrt PRIVATE openscrt_core openscrt_terminal openscrt_protocols
                                openscrt_transfer openscrt_credential openscrt_scripting
                                openscrt_config openscrt_ui
                                Qt6::Core Qt6::Gui Qt6::Widgets)

# MessageBoxA, for the one case a console build cannot cover by itself: launched by double-click,
# where the console window would flash and close before the user could read it.
if(WIN32)
    target_link_libraries(openscrt PRIVATE user32)
endif()

# Install layout. `cmake --install <build> --config Release --prefix <staging>` is the single
# place that decides what lands in an install tree; the release workflow stages with it and then
# runs windeployqt over the result, so the portable ZIP and the MSI contain byte-identical files.
install(TARGETS openscrt RUNTIME DESTINATION bin)

# Qt view layer. The renderer lives here rather than in openscrt_terminal so that the terminal
# emulation core stays free of Qt and remains headless-testable; the dirty-band logic that the
# renderer consumes lives in openscrt_terminal for the same reason.

if(NOT Qt6Gui_FOUND)
    message(FATAL_ERROR
        "openscrt_ui needs Qt6::Gui. Qt is installed by the CI Qt step; see docs/DEVELOPMENT_PLAN.md D2.")
endif()

add_library(openscrt_ui STATIC
    ModuleInfo.cpp
    renderer/GlyphAtlas.cpp
    renderer/TerminalRenderer.cpp)

target_include_directories(openscrt_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_ui
    PUBLIC openscrt_warnings
    PRIVATE openscrt_core openscrt_terminal Qt6::Core Qt6::Gui)

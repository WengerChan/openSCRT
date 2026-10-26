# Qt view layer. The renderer lives here rather than in openscrt_terminal so that the terminal
# emulation core stays free of Qt and remains headless-testable; the dirty-band logic that the
# renderer consumes lives in openscrt_terminal for the same reason.

if(NOT Qt6Gui_FOUND OR NOT Qt6Widgets_FOUND)
    message(FATAL_ERROR
        "openscrt_ui needs Qt6::Gui and Qt6::Widgets. Qt is installed by the CI Qt step; "
        "see docs/DEVELOPMENT_PLAN.md D2.")
endif()

# AUTOMOC for the widget's Q_OBJECT and signals.
set(CMAKE_AUTOMOC ON)

add_library(openscrt_ui STATIC
    ModuleInfo.cpp
    renderer/GlyphAtlas.cpp
    renderer/TerminalRenderer.cpp
    workspace/ButtonBar.cpp
    workspace/MainWindow.cpp
    workspace/CommandWindow.cpp
    workspace/KeePassSelectorDialog.cpp
    workspace/QuickConnectDialog.cpp
    workspace/SessionManagerWidget.cpp
    workspace/SerialHexView.cpp
    workspace/SftpDualPaneWidget.cpp
    workspace/TerminalCanvasWidget.cpp
    workspace/TerminalSession.cpp
    workspace/WorkspacePanes.cpp
    workspace/WorkspaceTabs.cpp)

target_include_directories(openscrt_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)

# The workspace layer is the composition root: it is the only place that knows both the
# session tree (openscrt_config) and the transport channels (openscrt_protocols). Both of
# those sit below it in the dependency graph and know nothing about the UI.
#
# Qt is PUBLIC because the public headers of this library include Qt headers themselves
# (<QMainWindow>, <QTabWidget>, ...). A consumer that includes them needs the Qt include
# directories, and a static library's PRIVATE link does not propagate them.
target_link_libraries(openscrt_ui
    PUBLIC openscrt_warnings Qt6::Core Qt6::Gui Qt6::Widgets
    PRIVATE openscrt_core openscrt_terminal openscrt_config openscrt_protocols
            openscrt_credential openscrt_transfer)

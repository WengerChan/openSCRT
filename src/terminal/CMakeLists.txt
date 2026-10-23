# Terminal emulation and rendering. Links core for the transport queue contract; no Qt yet so
# the emulator stays headless-testable (P1-T4 acceptance is decided by unit tests, not GUI).

add_library(openscrt_terminal STATIC
    ModuleInfo.cpp
    buffer/RingScrollbackBuffer.cpp
    buffer/ScreenGrid.cpp
    emulator/ControlSequenceParser.cpp
    emulator/ScreenSnapshot.cpp
    emulator/TerminalEmulator.cpp
    renderer/DamageTracker.cpp)

target_include_directories(openscrt_terminal PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_terminal PUBLIC openscrt_warnings PRIVATE openscrt_core)

# Test tree. Two CTest tiers so CI can gate correctness on every platform but keep the slow
# performance budget to a single Linux runner (docs/DEVELOPMENT_PLAN.md 2.3).

# Module mode, not CONFIG mode. The vcpkg gtest port builds GoogleTest and runs
# vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/GTest), but across the three runner
# images CMake's config-package search did not resolve GTestConfig.cmake even with the
# installed tree on CMAKE_PREFIX_PATH. CMake's own FindGTest module searches the library
# and include directories directly, which is exactly the vcpkg layout, and it has
# defined GTest::gtest / GTest::gtest_main since CMake 3.20.
find_package(GTest REQUIRED)

add_library(openscrt_test_support INTERFACE)
target_link_libraries(openscrt_test_support INTERFACE GTest::gtest GTest::gtest_main
                                                 openscrt_warnings)

# openscrt_add_test(<name> LABEL <unit|perf> SOURCES <...> LIBS <...>)
function(openscrt_add_test name)
    cmake_parse_arguments(ARG "" "LABEL" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src
                                                 ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE openscrt_test_support ${ARG_LIBS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABEL}" TIMEOUT 600)
endfunction()

openscrt_add_test(unit_spsc_ring_buffer
    LABEL unit
    SOURCES unit/SpscRingBufferTest.cpp
    LIBS openscrt_core)

openscrt_add_test(unit_secure_byte_array
    LABEL unit
    SOURCES unit/SecureByteArrayTest.cpp
    LIBS openscrt_core)

openscrt_add_test(unit_screen_grid
    LABEL unit
    SOURCES unit/ScreenGridTest.cpp
    LIBS openscrt_terminal)

openscrt_add_test(unit_ring_scrollback_buffer
    LABEL unit
    SOURCES unit/RingScrollbackBufferTest.cpp
    LIBS openscrt_terminal)

# The renderer needs a QGuiApplication; the offscreen platform plugin makes that work in CI
# without a display server.
openscrt_add_test(unit_terminal_canvas_widget
    LABEL unit
    SOURCES unit/TerminalCanvasWidgetTest.cpp
    LIBS openscrt_ui Qt6::Gui Qt6::Widgets)
set_tests_properties(unit_terminal_canvas_widget PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

openscrt_add_test(unit_terminal_renderer
    LABEL unit
    SOURCES unit/TerminalRendererTest.cpp
    LIBS openscrt_ui Qt6::Gui)
set_tests_properties(unit_terminal_renderer PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

openscrt_add_test(unit_session_profile
    LABEL unit
    SOURCES unit/SessionProfileTest.cpp
    LIBS openscrt_config)

openscrt_add_test(unit_config_manager
    LABEL unit
    SOURCES unit/ConfigManagerTest.cpp
    LIBS openscrt_config Qt6::Core)

# The window shell needs a QApplication; the offscreen platform plugin makes that work in CI
# without a display server.
openscrt_add_test(unit_main_window
    LABEL unit
    SOURCES unit/MainWindowTest.cpp
    LIBS openscrt_ui openscrt_config Qt6::Gui Qt6::Widgets)
set_tests_properties(unit_main_window PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

openscrt_add_test(unit_ssh2_channel
    LABEL unit
    SOURCES unit/Ssh2ChannelTest.cpp
    LIBS openscrt_protocols)

openscrt_add_test(unit_local_pty_channel
    LABEL unit
    SOURCES unit/LocalPtyChannelTest.cpp
    LIBS openscrt_protocols)

openscrt_add_test(unit_damage_tracker
    LABEL unit
    SOURCES unit/DamageTrackerTest.cpp
    LIBS openscrt_terminal)

openscrt_add_test(unit_frame_coalescer
    LABEL unit
    SOURCES unit/FrameCoalescerTest.cpp
    LIBS openscrt_terminal)

openscrt_add_test(unit_terminal_emulator
    LABEL unit
    SOURCES unit/TerminalEmulatorTest.cpp
    LIBS openscrt_terminal)

openscrt_add_test(unit_control_sequence_parser
    LABEL unit
    SOURCES unit/ControlSequenceParserTest.cpp
    LIBS openscrt_terminal)

# P1-A5: scrollback write throughput and lookup latency under a realistic 128k-line working
# set. Iteration count is scaled by OPENCRT_PERF_ITERS so the shared CI runner stays inside
# its budget while a dedicated runner can raise it.
# P1-A3 lives in the perf tier so its numbers are echoed into the run log: ctest only shows a
# passing test's output under -V, and CI runs the perf tier with -V.
openscrt_add_test(perf_render_budget
    LABEL perf
    SOURCES perf/RenderBudgetTest.cpp
    LIBS openscrt_ui Qt6::Gui)
set_tests_properties(perf_render_budget PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

openscrt_add_test(perf_scrollback_throughput
    LABEL perf
    SOURCES perf/ScrollbackThroughputTest.cpp
    LIBS openscrt_terminal)

# The GUI shell smoke test. It runs the real executable with --self-test, which builds the
# window, shows it and checks the docks and the menu bar, all on the offscreen platform.
# Without this the only thing CI proves about the shell is that it compiles.
add_test(NAME app_gui_self_test COMMAND openscrt --self-test)
set_tests_properties(app_gui_self_test PROPERTIES
    LABELS "unit"
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 120)
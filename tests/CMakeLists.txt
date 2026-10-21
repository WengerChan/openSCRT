# Test tree. Two CTest tiers so CI can gate correctness on every platform but keep the slow
# performance budget to a single Linux runner (docs/DEVELOPMENT_PLAN.md 2.3).

find_package(GTest CONFIG REQUIRED)

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

openscrt_add_test(unit_control_sequence_parser
    LABEL unit
    SOURCES unit/ControlSequenceParserTest.cpp
    LIBS openscrt_terminal)

# P1-A5: scrollback write throughput and lookup latency under a realistic 128k-line working
# set. Iteration count is scaled by OPENCRT_PERF_ITERS so the shared CI runner stays inside
# its budget while a dedicated runner can raise it.
openscrt_add_test(perf_scrollback_throughput
    LABEL perf
    SOURCES perf/ScrollbackThroughputTest.cpp
    LIBS openscrt_terminal)
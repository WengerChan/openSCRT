cmake_minimum_required(VERSION 3.28)

project(openscrt
    VERSION 0.1.2
    DESCRIPTION "SecureCRT-alike native cross-platform terminal with dual-mode KeePass credentials"
    LANGUAGES CXX)

# --------------------------------------------------------------------------------------
# Build options
# --------------------------------------------------------------------------------------
option(OPENCRT_BUILD_TESTS "Build the unit / integration / performance test suites" ON)
option(OPENCRT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(OPENCRT_ENABLE_COVERAGE "Instrument the build for gcov/lcov coverage reporting" OFF)

# Coverage instrumentation. Off by default: it forces -O0 -g and would slow the normal
# three-platform matrix down. CI turns it on for the Linux leg only and filters out
# src/app (a non-test executable, so it would always report 0%).
if(OPENCRT_ENABLE_COVERAGE AND NOT MSVC)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The Windows floor, as a compile-time contract.
#
# The product's floor is Windows 10 1809 (build 17763): the Qt libraries in the package
# import kernel32 exports that older systems do not export, so a package built today cannot
# start on anything older. Declaring the floor here means a call to something newer fails to
# compile instead of quietly raising the floor for every user of the package -- and CI can
# never notice that on its own, because the runner is newer than every system we support.
#
# Our own code is kept clean down to 1607, so that a Qt with a lower floor would lower the
# product's floor with it. The import table gate in CI measures that instead of trusting
# this declaration: a declaration says what we believe, the import table says what we built.
if(MSVC)
    add_compile_definitions(WINVER=0x0A04 _WIN32_WINNT=0x0A04)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Everything lands in one place so ccache/artifact paths stay predictable.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --------------------------------------------------------------------------------------
# Warning / formatting policy (CI gate, see docs/DEVELOPMENT_PLAN.md 2.2)
# --------------------------------------------------------------------------------------
add_library(openscrt_warnings INTERFACE)
if(MSVC)
    target_compile_options(openscrt_warnings INTERFACE /W4 /permissive-)
    if(OPENCRT_WARNINGS_AS_ERRORS)
        target_compile_options(openscrt_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(openscrt_warnings INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(openscrt_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
    if(OPENCRT_WARNINGS_AS_ERRORS)
        target_compile_options(openscrt_warnings INTERFACE -Werror)
    endif()
endif()

# --------------------------------------------------------------------------------------
# Qt6
#
# Phase 0 deliberately links Qt6::Core only so the CI gate stays fast while the core
# data structures are built out. Widgets / SerialPort are pulled in when their modules
# become non-empty (Phase 1+).
# --------------------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(Qt6 QUIET COMPONENTS Gui Widgets SerialPort)
set(OPENCRT_QT_GUI_AVAILABLE ${Qt6Gui_FOUND})
set(OPENCRT_QT_WIDGETS_AVAILABLE ${Qt6Widgets_FOUND})
set(OPENCRT_QT_SERIALPORT_AVAILABLE ${Qt6SerialPort_FOUND})

# --------------------------------------------------------------------------------------
# libsodium (pkg-config keeps us working across vcpkg / distro layouts)
# --------------------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SODIUM QUIET libsodium)
endif()

if(SODIUM_FOUND)
    add_library(openscrt_sodium INTERFACE)
    target_include_directories(openscrt_sodium SYSTEM INTERFACE ${SODIUM_INCLUDE_DIRS})
    target_link_libraries(openscrt_sodium INTERFACE ${SODIUM_LINK_LIBRARIES})
    target_link_directories(openscrt_sodium INTERFACE ${SODIUM_LIBRARY_DIRS})
else()
    add_library(openscrt_sodium INTERFACE)
    target_link_libraries(openscrt_sodium INTERFACE sodium)
endif()
add_library(libsodium::openscrt ALIAS openscrt_sodium)

# --------------------------------------------------------------------------------------
# Source tree
# --------------------------------------------------------------------------------------
add_subdirectory(src)

if(OPENCRT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "openSCRT ${PROJECT_VERSION}")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  warnings-as-err : ${OPENCRT_WARNINGS_AS_ERRORS}")
# Printed because it is the only honest record of *which* Qt won. install-qt-action exports
# QT_ROOT_DIR (not Qt6_DIR), and CI feeds that into CMAKE_PREFIX_PATH; a silent change in the
# discovery mechanism would otherwise show up as an unexplained Qt version bump.
message(STATUS "  Qt6 config      : ${Qt6_DIR}")
message(STATUS "  Qt widgets      : ${OPENCRT_QT_WIDGETS_AVAILABLE}")
message(STATUS "  Qt serialport   : ${OPENCRT_QT_SERIALPORT_AVAILABLE}")
message(STATUS "  tests           : ${OPENCRT_BUILD_TESTS}")
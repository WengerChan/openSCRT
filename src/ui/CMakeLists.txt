# Qt Widgets view layer. Populated in P1-T12; the target exists from Phase 0 so the CI gate
# covers the Qt link line on all three platforms.

add_library(openscrt_ui STATIC ModuleInfo.cpp)

target_include_directories(openscrt_ui PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(openscrt_ui PUBLIC openscrt_warnings PRIVATE openscrt_core openscrt_terminal Qt6::Core)

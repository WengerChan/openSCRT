# Source tree. Each module is an independent static library so it can be unit tested and
# upgraded without dragging in the rest of the application (docs/DEVELOPMENT_PLAN.md 2.3).

add_subdirectory(core)
add_subdirectory(terminal)
add_subdirectory(protocols)
add_subdirectory(transfer)
add_subdirectory(credential)
add_subdirectory(scripting)
add_subdirectory(config)
add_subdirectory(ui)
add_subdirectory(app)
add_library(rummage_platform
    src/code_stats.cpp
    src/code_report.cpp
    src/program.cpp
    src/action_handoff.cpp
    src/archives.cpp
    src/about.cpp
    src/applications.cpp
    src/bus_loop.cpp
    src/desktop_entry.cpp
    src/exec.cpp
    src/gvfs.cpp
    src/file_manager.cpp
    src/file_manager_config.cpp
    src/file_picker_portal.cpp
    src/terminal_navigation.cpp
    src/launcher.cpp
    src/notifications.cpp
    src/program_output.cpp
    src/opener.cpp
    src/portal.cpp
    src/udisks.cpp
    src/volumes.cpp)
add_library(rummage::platform ALIAS rummage_platform)

target_include_directories(rummage_platform PUBLIC include)
# thumbs: file_uri(), the file:// URIs %u and %U take, escaped as GLib does.
target_link_libraries(rummage_platform
    PUBLIC rummage::core rummage::fs
    PRIVATE rummage::thumbs rummage::warnings nlohmann_json::nlohmann_json PkgConfig::SDBUSCPP ${CMAKE_DL_LIBS})

if(TOKEI_EXECUTABLE)
    target_compile_definitions(rummage_platform PRIVATE RUMMAGE_TOKEI_PROGRAM="${TOKEI_EXECUTABLE}")
endif()

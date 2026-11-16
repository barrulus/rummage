# Embed the installed reference so first-run setup also works from a build tree.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/docs/config.example.toml")
file(READ "${PROJECT_SOURCE_DIR}/docs/config.example.toml" RUMMAGE_DEFAULT_CONFIG)
configure_file(config_template.hpp.in generated/config_template.hpp @ONLY)

# Translations: every data/i18n/<code>.toml is compiled in, so a new language needs only its
# file (docs/translations.md). Written without @-substitution: texts may hold "@".
file(GLOB RUMMAGE_CATALOG_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/data/i18n/*.toml")
list(SORT RUMMAGE_CATALOG_FILES)
set(RUMMAGE_CATALOGS_CPP "// Generated from data/i18n/*.toml by src/core/CMakeLists.txt.\n")
string(APPEND RUMMAGE_CATALOGS_CPP "// NOLINTBEGIN\n#include <array>\n\n#include \"i18n_catalogs.hpp\"\n\n")
string(APPEND RUMMAGE_CATALOGS_CPP "namespace rummage::i18n::detail {\n\n")
string(APPEND RUMMAGE_CATALOGS_CPP "std::span<const Catalog> catalogs() {\n")
set(RUMMAGE_CATALOG_ENTRIES "")
set(RUMMAGE_CATALOG_COUNT 0)
foreach(catalog IN LISTS RUMMAGE_CATALOG_FILES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${catalog}")
    get_filename_component(code "${catalog}" NAME_WE)
    if(NOT code MATCHES "^[a-z][a-z][a-z]?(_[A-Z][A-Z])?$")
        message(FATAL_ERROR "data/i18n/${code}.toml: name it after a language code, like de or pt_BR")
    endif()
    file(READ "${catalog}" text)
    string(FIND "${text}" ")rummage_i18n" clash)
    if(NOT clash EQUAL -1)
        message(FATAL_ERROR "${catalog} holds the text \")rummage_i18n\"")
    endif()
    string(APPEND RUMMAGE_CATALOG_ENTRIES
        "        Catalog{\"${code}\", R\"rummage_i18n(${text})rummage_i18n\"},\n")
    math(EXPR RUMMAGE_CATALOG_COUNT "${RUMMAGE_CATALOG_COUNT} + 1")
endforeach()
string(APPEND RUMMAGE_CATALOGS_CPP
    "    static constexpr std::array<Catalog, ${RUMMAGE_CATALOG_COUNT}> kCatalogs{\n"
    "${RUMMAGE_CATALOG_ENTRIES}    };\n    return kCatalogs;\n}\n\n"
    "}  // namespace rummage::i18n::detail\n// NOLINTEND\n")
# Rewritten only when it changes, so a reconfigure does not rebuild it.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/i18n_catalogs.cpp.new" "${RUMMAGE_CATALOGS_CPP}")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/i18n_catalogs.cpp.new"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/i18n_catalogs.cpp" COPYONLY)

add_library(rummage_core
    src/actions.cpp
    src/config.cpp
    src/config_edit.cpp
    src/event_loop.cpp
    src/file.cpp
    src/file_picker.cpp
    src/file_watcher.cpp
    src/i18n.cpp
    src/log.cpp
    src/memory.cpp
    src/worker_pool.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/i18n_catalogs.cpp")
add_library(rummage::core ALIAS rummage_core)

target_include_directories(rummage_core PUBLIC include PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(rummage_core PRIVATE PkgConfig::TOMLPLUSPLUS rummage::warnings)

pkg_check_modules(ASSIMP REQUIRED IMPORTED_TARGET assimp)
# Optional untrusted-model parsing stays in the ordinary preview cage.
add_executable(rummage-model backend.cpp
    ../isolate/src/helper_main.cpp ../isolate/src/protocol.cpp ../isolate/src/sandbox.cpp)
target_include_directories(rummage-model PRIVATE ../isolate/src)
target_link_libraries(rummage-model PRIVATE rummage::picture rummage::text rummage::archive
    PkgConfig::ASSIMP rummage::warnings)
set_target_properties(rummage-model PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
install(TARGETS rummage-model RUNTIME DESTINATION libexec/rummage COMPONENT model)
configure_file("${PROJECT_SOURCE_DIR}/data/engines/model.toml"
    "${PROJECT_BINARY_DIR}/engines/model.toml" COPYONLY)
install(FILES "${PROJECT_SOURCE_DIR}/data/engines/model.toml"
    DESTINATION share/rummage/engines COMPONENT model)

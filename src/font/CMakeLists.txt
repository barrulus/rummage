# Optional untrusted-font parsing stays in the ordinary preview cage.
add_executable(rummage-font backend.cpp
    ../isolate/src/helper_main.cpp ../isolate/src/protocol.cpp ../isolate/src/sandbox.cpp)
target_include_directories(rummage-font PRIVATE ../isolate/src)
target_link_libraries(rummage-font PRIVATE rummage::picture rummage::text rummage::archive
    PkgConfig::FREETYPE rummage::warnings)
set_target_properties(rummage-font PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
install(TARGETS rummage-font RUNTIME DESTINATION libexec/rummage COMPONENT font)
configure_file("${PROJECT_SOURCE_DIR}/data/engines/font.toml"
    "${PROJECT_BINARY_DIR}/engines/font.toml" COPYONLY)
install(FILES "${PROJECT_SOURCE_DIR}/data/engines/font.toml"
    DESTINATION share/rummage/engines COMPONENT font)

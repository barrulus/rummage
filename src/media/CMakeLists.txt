# The only target that links FFmpeg. Bundled in the standard package, with runtime UI controls.
add_executable(rummage-media backend.cpp
    ../isolate/src/helper_main.cpp ../isolate/src/protocol.cpp ../isolate/src/sandbox.cpp)
target_include_directories(rummage-media PRIVATE ../isolate/src)
target_link_libraries(rummage-media PRIVATE rummage::picture rummage::text rummage::archive
    PkgConfig::AVFORMAT PkgConfig::AVCODEC PkgConfig::AVUTIL PkgConfig::SWSCALE rummage::warnings)
set_target_properties(rummage-media PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
install(TARGETS rummage-media RUNTIME DESTINATION libexec/rummage COMPONENT media)
configure_file("${PROJECT_SOURCE_DIR}/data/engines/media.toml"
    "${PROJECT_BINARY_DIR}/engines/media.toml" COPYONLY)
install(FILES "${PROJECT_SOURCE_DIR}/data/engines/media.toml"
    DESTINATION share/rummage/engines COMPONENT media)

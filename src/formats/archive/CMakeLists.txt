# The archive family (ROADMAP 0.9.0): zip, tar, tar.gz and .gz, read through descriptors. Linked
# into the isolated helper (and tests and development tools), never into rummage. zlib inflates.
add_library(rummage_formats_archive src/archive.cpp src/stream.cpp src/tar.cpp src/zip.cpp)
add_library(rummage::formats::archive ALIAS rummage_formats_archive)
target_include_directories(rummage_formats_archive PUBLIC include)
target_link_libraries(rummage_formats_archive
    PUBLIC rummage::archive
    PRIVATE PkgConfig::ZLIB rummage::warnings)

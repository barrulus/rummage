# What rummage does with an archive (ROADMAP 0.9.0): its entries as plain data. No reader of
# archives: those are in formats/archive, run by the base engine.
add_library(rummage_archive src/archive.cpp)
add_library(rummage::archive ALIAS rummage_archive)
target_include_directories(rummage_archive PUBLIC include)
target_link_libraries(rummage_archive PRIVATE rummage::warnings)

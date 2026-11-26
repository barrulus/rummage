add_library(rummage_fs
    src/annotations.cpp
    src/archive_folder.cpp
    src/archive_origin.cpp
    src/advanced_archive.cpp
    src/compress.cpp
    src/extract.cpp
    src/batch_rename.cpp
    src/name_pattern.cpp
    src/permissions.cpp
    src/directory.cpp
    src/directory_model.cpp
    src/directory_watcher.cpp
    src/erase.cpp
    src/filesystem.cpp
    src/folder_icon.cpp
    src/folder_icon_store.cpp
    src/icon.cpp
    src/links.cpp
    src/group.cpp
    src/path_completion.cpp
    src/insights.cpp
    src/measure.cpp
    src/mime.cpp
    src/operations.cpp
    src/recent.cpp
    src/places.cpp
    src/subfolders.cpp
    src/sort.cpp
    src/transfer.cpp
    src/transfer_queue.cpp
    src/trash.cpp
    src/walk.cpp)
add_library(rummage::fs ALIAS rummage_fs)

target_include_directories(rummage_fs PUBLIC include)
target_link_libraries(rummage_fs PUBLIC rummage::core rummage::archive PRIVATE PkgConfig::ZLIB PkgConfig::RE2 rummage::warnings)

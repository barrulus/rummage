add_library(rummage_photo_client src/client.cpp src/source_watcher.cpp src/work_diagnostics.cpp)
add_library(rummage::photo_client ALIAS rummage_photo_client)
target_include_directories(rummage_photo_client PUBLIC include)
target_link_libraries(rummage_photo_client PUBLIC nlohmann_json::nlohmann_json
    PRIVATE rummage::core ${CMAKE_DL_LIBS} rummage::warnings rummage::picture rummage::thumbs
            rummage::fs rummage::engines)

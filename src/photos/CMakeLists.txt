find_package(SQLite3 REQUIRED)
add_library(rummage_photos SHARED src/catalog.cpp)
set_target_properties(rummage_photos PROPERTIES OUTPUT_NAME rummage-photos
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
target_include_directories(rummage_photos PUBLIC include)
target_link_libraries(rummage_photos PRIVATE SQLite::SQLite3 nlohmann_json::nlohmann_json rummage::warnings)
install(TARGETS rummage_photos LIBRARY DESTINATION libexec/rummage COMPONENT photos)

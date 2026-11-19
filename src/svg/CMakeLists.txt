# One librsvg/Cairo adapter, shared by theme icons, file previews and tools.
add_library(rummage_svg src/render.cpp)
add_library(rummage::svg ALIAS rummage_svg)
target_include_directories(rummage_svg PUBLIC include)
target_link_libraries(rummage_svg PUBLIC rummage::picture
                                 PRIVATE PkgConfig::RSVG rummage::warnings)

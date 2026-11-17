# The optional git integration (RUMMAGE_GIT): finds the repository a folder is in and reads its
# state from the git program (`git status --porcelain=v2`), so rummage links no git library.
# Without git in the PATH it stays out of sight.
add_library(rummage_git
    src/actions.cpp
    src/decorator.cpp
    src/repository.cpp
    src/status.cpp)
add_library(rummage::git ALIAS rummage_git)

target_include_directories(rummage_git PUBLIC include)
target_link_libraries(rummage_git PUBLIC rummage::core rummage::fs rummage::views
    PRIVATE rummage::platform rummage::warnings)

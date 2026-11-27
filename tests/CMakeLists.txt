add_executable(rummage-code-fixture code_stats_fixture.cpp)
target_link_libraries(rummage-code-fixture PRIVATE rummage::warnings)
add_executable(rummage-program-fixture program_fixture.cpp)
target_link_libraries(rummage-program-fixture PRIVATE rummage::warnings)

add_executable(rummage-i18n-tests i18n_runtime_test.cpp
    ${PROJECT_SOURCE_DIR}/src/core/src/i18n.cpp
    ${PROJECT_SOURCE_DIR}/src/core/src/i18n_rules.cpp
    ${PROJECT_SOURCE_DIR}/src/core/src/log.cpp)
target_include_directories(rummage-i18n-tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/core/include ${PROJECT_SOURCE_DIR}/src/core/src)
target_link_libraries(rummage-i18n-tests PRIVATE doctest::doctest PkgConfig::TOMLPLUSPLUS rummage::warnings)
add_test(NAME i18n-runtime COMMAND rummage-i18n-tests)

add_executable(rummage-tests
    batch_rename_test.cpp
    permissions_test.cpp
    program_test.cpp
    actions_test.cpp
    archives_test.cpp
    compress_test.cpp
    extract_test.cpp
    archive_test.cpp
    archive_folder_test.cpp
    main.cpp
    photo_client_test.cpp
    about_test.cpp
    app_icon_test.cpp
    breadcrumb_test.cpp
    clipboard_test.cpp
    column_view_test.cpp
    command_palette_test.cpp
    config_test.cpp
    content_map_test.cpp
    settings_test.cpp
    settings_editors_test.cpp
    desktop_palette_test.cpp
    context_menu_test.cpp
    menu_editor_test.cpp
    details_view_test.cpp
    devices_test.cpp
    engines_test.cpp
    drag_drop_test.cpp
    directory_model_test.cpp
    directory_test.cpp
    dng_test.cpp
    event_loop_test.cpp
    feature_integration_test.cpp
    file_watcher_test.cpp
    format_test.cpp
    folder_icon_test.cpp
    i18n_test.cpp
    icon_test.cpp
    icon_theme_test.cpp
    svg_test.cpp
    svg_preview_test.cpp
    svg_features_test.cpp
    svg_layers_test.cpp
    svg_effects_test.cpp
    helpers_test.cpp
    sandbox_test.cpp
    tiff_test.cpp
    image_test.cpp
    heif_test.cpp
    filesystem_test.cpp
    links_test.cpp
    group_test.cpp
    path_completion_test.cpp
    load_test.cpp
    insights_test.cpp
    code_stats_test.cpp
    code_report_test.cpp
    measure_test.cpp
    memory_test.cpp
    mime_test.cpp
    navigation_test.cpp
    palette_test.cpp
    kde_palette_test.cpp
    paste_test.cpp
    places_test.cpp
    remote_test.cpp
    pointer_shapes_test.cpp
    tabs_test.cpp
    panes_test.cpp
    markdown_test.cpp
    markdown_view_test.cpp
    text_test.cpp
    text_selection_test.cpp
    terminal_navigation_test.cpp
    platform_test.cpp
    preview_test.cpp
    info_pane_test.cpp
    quick_look_test.cpp
    rename_test.cpp
    glyph_test.cpp
    renderer_runs_test.cpp
    motion_test.cpp
    scrollbar_test.cpp
    scroller_test.cpp
    shortcuts_test.cpp
    sort_test.cpp
    syntax_test.cpp
    syntax_colors_test.cpp
    state_test.cpp
    thumbs_test.cpp
    title_bar_test.cpp
    erase_test.cpp
    erase_view_test.cpp
    file_finder_test.cpp
    file_manager_test.cpp
    file_picker_test.cpp
    transfer_test.cpp
    trash_test.cpp
    trash_view_test.cpp
    undo_test.cpp
    worker_pool_test.cpp
    xcursor_test.cpp)
target_link_libraries(rummage-tests PRIVATE rummage::svg rummage::core rummage::theme rummage::fs rummage::views rummage::picture rummage::formats rummage::formats::image rummage::formats::text rummage::formats::archive rummage::text rummage::thumbs rummage::preview rummage::engines rummage::isolate rummage::platform doctest::doctest rummage::warnings)
target_link_libraries(rummage-tests PRIVATE rummage::photo_client)
if(RUMMAGE_PHOTOS)
    target_compile_definitions(rummage-tests PRIVATE
        RUMMAGE_PHOTOS_TEST_MODULE="$<TARGET_FILE:rummage_photos>")
    add_dependencies(rummage-tests rummage_photos)
    add_executable(rummage-photos-tests photos_test.cpp)
    target_link_libraries(rummage-photos-tests PRIVATE rummage_photos doctest::doctest
        nlohmann_json::nlohmann_json rummage::warnings)
    add_test(NAME photos-catalog COMMAND rummage-photos-tests)
    add_executable(rummage-photo-recovery-tests photo_recovery_test.cpp)
    target_link_libraries(rummage-photo-recovery-tests PRIVATE rummage::photo_client rummage::fs
        doctest::doctest rummage::warnings)
    target_compile_definitions(rummage-photo-recovery-tests PRIVATE
        RUMMAGE_PHOTOS_TEST_MODULE="$<TARGET_FILE:rummage_photos>")
    add_dependencies(rummage-photo-recovery-tests rummage_photos)
    add_test(NAME photos-recovery COMMAND rummage-photo-recovery-tests)
    add_executable(rummage-photo-watcher-tests photos_watcher_test.cpp)
    target_link_libraries(rummage-photo-watcher-tests PRIVATE rummage::photo_client rummage::fs rummage::thumbs
        rummage::core doctest::doctest rummage::warnings)
    target_compile_definitions(rummage-photo-watcher-tests PRIVATE
        RUMMAGE_PHOTOS_TEST_MODULE="$<TARGET_FILE:rummage_photos>")
    add_dependencies(rummage-photo-watcher-tests rummage_photos)
    add_test(NAME photos-watcher COMMAND rummage-photo-watcher-tests)
    set_tests_properties(photos-watcher PROPERTIES TIMEOUT 60)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME photos-migration COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/photos_migration.py" "$<TARGET_FILE:rummage_photos>")
    # ctypes loads the instrumented module into an uninstrumented Python host. ASan must
    # already be present before that dlopen; use this toolchain's runtime, not the host's.
    if(RUMMAGE_SANITIZER MATCHES "address" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(_photos_asan_name "libclang_rt.asan-${CMAKE_SYSTEM_PROCESSOR}.so")
        else()
            set(_photos_asan_name "libasan.so")
        endif()
        execute_process(COMMAND "${CMAKE_CXX_COMPILER}" "-print-file-name=${_photos_asan_name}"
            OUTPUT_VARIABLE _photos_asan_runtime OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        if(NOT EXISTS "${_photos_asan_runtime}")
            message(FATAL_ERROR "Cannot locate sanitizer runtime for Python migration test: ${_photos_asan_name}")
        endif()
        set_tests_properties(photos-migration PROPERTIES ENVIRONMENT
            "LD_PRELOAD=${_photos_asan_runtime};ASAN_OPTIONS=detect_leaks=0:abort_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endif()

# The helper protocol's private header, for its tests; the image family's private readers (TIFF,
# DNG, lossless JPEG), for theirs.
target_include_directories(rummage-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src
                                                 ${PROJECT_SOURCE_DIR}/src/formats/image/src)

# LibRaw, when installed, as the reference for the DNG reader's values (tests only: never linked
# into rummage or its helper).
pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw_r)
if(LIBRAW_FOUND)
    target_sources(rummage-tests PRIVATE dng_libraw_test.cpp)
    target_link_libraries(rummage-tests PRIVATE PkgConfig::LIBRAW)
endif()

# exiv2, when installed, as the reference for the metadata readers that replace it (ROADMAP 2e;
# tests only: never linked into rummage or its helper).
pkg_check_modules(EXIV2_REFERENCE QUIET IMPORTED_TARGET exiv2)
if(EXIV2_REFERENCE_FOUND)
    target_sources(rummage-tests PRIVATE exiv2_reference.cpp exiv2_reference_test.cpp)
    target_link_libraries(rummage-tests PRIVATE PkgConfig::EXIV2_REFERENCE)
    # rummage's reader against exiv2's, timed (tools, not tests).
    add_executable(rummage-bench-previews bench_previews.cpp exiv2_reference.cpp)
    target_include_directories(rummage-bench-previews PRIVATE ${PROJECT_SOURCE_DIR}/src/formats/image/src)
    target_link_libraries(rummage-bench-previews
        PRIVATE rummage::formats::image PkgConfig::EXIV2_REFERENCE rummage::warnings)
endif()

# A preview helper that misbehaves on purpose (crash, hang, nonsense), and the real one, for
# tests/helpers_test.cpp.
add_executable(rummage-fake-preview fake_preview_helper.cpp
                                    ${PROJECT_SOURCE_DIR}/src/isolate/src/protocol.cpp)
target_include_directories(rummage-fake-preview PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(rummage-fake-preview PRIVATE rummage::picture rummage::text rummage::archive rummage::warnings)

# The sandbox probe: the preview helper's cage, then one forbidden (or allowed) thing.
add_executable(rummage-sandbox-probe sandbox_probe.cpp
                                     ${PROJECT_SOURCE_DIR}/src/isolate/src/sandbox.cpp)
target_include_directories(rummage-sandbox-probe PRIVATE ${PROJECT_SOURCE_DIR}/src/isolate/src)
target_link_libraries(rummage-sandbox-probe PRIVATE rummage::warnings)
add_dependencies(rummage-tests rummage-code-fixture rummage rummage-program-fixture rummage-preview rummage-fake-preview rummage-sandbox-probe)
# The MIME database the tests read: shared-mime-info as pkg-config finds it at build time, not
# whatever $XDG_DATA_DIRS says when they run (NixOS keeps it outside /usr/share, Flatpak adds
# its own). tests/main.cpp points $XDG_DATA_DIRS at it.
pkg_get_variable(RUMMAGE_MIME_PREFIX shared-mime-info prefix)
if(NOT RUMMAGE_MIME_PREFIX OR NOT EXISTS "${RUMMAGE_MIME_PREFIX}/share/mime/globs2")
    message(FATAL_ERROR "The tests need shared-mime-info (its pkg-config file and "
                        "share/mime/globs2); install it, or configure with "
                        "-DRUMMAGE_BUILD_TESTS=OFF")
endif()
# The shipped example config must parse clean and match the built-in defaults.
target_compile_definitions(rummage-tests
    PRIVATE RUMMAGE_CODE_FIXTURE="$<TARGET_FILE:rummage-code-fixture>"
            RUMMAGE_EXECUTABLE="$<TARGET_FILE:rummage>"
            RUMMAGE_TEST_PROGRAM="$<TARGET_FILE:rummage-program-fixture>"
            RUMMAGE_EXAMPLE_CONFIG="${PROJECT_SOURCE_DIR}/docs/config.example.toml"
            RUMMAGE_TEST_DATA="${PROJECT_SOURCE_DIR}/tests/data"
            RUMMAGE_APP_ICON="${PROJECT_SOURCE_DIR}/data/rummage.svg"
            RUMMAGE_ENGINES="${PROJECT_SOURCE_DIR}/data/engines"
            RUMMAGE_MIME_DATA="${RUMMAGE_MIME_PREFIX}/share"
            RUMMAGE_PREVIEW_HELPER="$<TARGET_FILE:rummage-preview>"
            RUMMAGE_FAKE_HELPER="$<TARGET_FILE:rummage-fake-preview>"
            RUMMAGE_SANDBOX_PROBE="$<TARGET_FILE:rummage-sandbox-probe>")
if(RUMMAGE_SANITIZER)
    target_sources(rummage-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/core/src/lsan_suppressions.cpp)
endif()

if(RUMMAGE_PHOTOS)
    # Native photo workflows have their own executable so UI iteration does not rebuild the
    # unrelated format, filesystem and preview test translation units.
    add_executable(rummage-photo-ui-tests main.cpp photo_library_ui_test.cpp)
    target_link_libraries(rummage-photo-ui-tests PRIVATE
        $<TARGET_PROPERTY:rummage-tests,LINK_LIBRARIES>)
    target_compile_definitions(rummage-photo-ui-tests PRIVATE
        $<TARGET_PROPERTY:rummage-tests,COMPILE_DEFINITIONS>)
    if(RUMMAGE_SANITIZER)
        target_sources(rummage-photo-ui-tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/core/src/lsan_suppressions.cpp)
    endif()
    add_dependencies(rummage-photo-ui-tests rummage rummage-preview rummage_photos)
    add_test(NAME photos-ui COMMAND rummage-photo-ui-tests)
endif()

# Allocator thresholds depend on earlier allocations: give memory accounting its own process.
add_test(NAME rummage-tests COMMAND rummage-tests "--source-file-exclude=*memory_test.cpp")
add_test(NAME memory-tests COMMAND rummage-tests "--source-file=*memory_test.cpp")

add_executable(rummage-portal-tests file_picker_portal_test.cpp)
target_link_libraries(rummage-portal-tests PRIVATE rummage::platform PkgConfig::SDBUSCPP rummage::warnings)
add_executable(rummage-desktop-portal-tests desktop_portal_test.cpp)
target_link_libraries(rummage-desktop-portal-tests PRIVATE rummage::platform PkgConfig::SDBUSCPP rummage::warnings)
add_executable(rummage-notification-tests notifications_test.cpp)
target_include_directories(rummage-notification-tests PRIVATE ${PROJECT_SOURCE_DIR}/src/platform/src)
target_link_libraries(rummage-notification-tests PRIVATE rummage::platform PkgConfig::SDBUSCPP rummage::warnings)
add_test(NAME file-manager-cli COMMAND "${CMAKE_COMMAND}"
    "-DRUMMAGE_EXECUTABLE=$<TARGET_FILE:rummage>"
    "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/file-manager-cli-scratch"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/file_manager_cli.cmake")
set_tests_properties(file-manager-cli PROPERTIES TIMEOUT 30)
add_executable(rummage-file-manager-tests file_manager_bus_test.cpp)
target_link_libraries(rummage-file-manager-tests PRIVATE rummage::platform PkgConfig::SDBUSCPP rummage::warnings)
find_program(DBUS_RUN_SESSION dbus-run-session)
find_program(DBUS_SEND dbus-send)
if(DBUS_RUN_SESSION AND DBUS_SEND)
    add_test(NAME file-manager-activation COMMAND "${CMAKE_COMMAND}"
        "-DRUMMAGE_EXECUTABLE=$<TARGET_FILE:rummage>"
        "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/file-manager-activation-scratch"
        "-DDBUS_RUN_SESSION=${DBUS_RUN_SESSION}" "-DDBUS_SEND=${DBUS_SEND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/file_manager_activation.cmake")
    set_tests_properties(file-manager-activation PROPERTIES TIMEOUT 40)
endif()
if(DBUS_RUN_SESSION)
    add_test(NAME desktop-portal COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:rummage-desktop-portal-tests>)
    set_tests_properties(desktop-portal PROPERTIES TIMEOUT 30)
    add_test(NAME file-manager COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:rummage-file-manager-tests>)
    set_tests_properties(file-manager PROPERTIES TIMEOUT 15)
    add_test(NAME file-picker-portal COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:rummage-portal-tests>)
    set_tests_properties(file-picker-portal PROPERTIES TIMEOUT 30)
    add_test(NAME action-notifications COMMAND "${DBUS_RUN_SESSION}" -- $<TARGET_FILE:rummage-notification-tests>)
    set_tests_properties(action-notifications PROPERTIES TIMEOUT 15)
endif()

if(RUMMAGE_MEDIA)
    add_executable(rummage-media-tests media_test.cpp)
    target_link_libraries(rummage-media-tests PRIVATE doctest::doctest rummage::isolate
        rummage::thumbs rummage::preview rummage::warnings)
    target_compile_definitions(rummage-media-tests PRIVATE
        RUMMAGE_MEDIA_HELPER="$<TARGET_FILE:rummage-media>"
        RUMMAGE_BASE_HELPER="$<TARGET_FILE:rummage-preview>"
        RUMMAGE_FAKE_HELPER="$<TARGET_FILE:rummage-fake-preview>"
        RUMMAGE_MEDIA_DATA="${PROJECT_SOURCE_DIR}/tests/data/media"
        RUMMAGE_IMAGE_DATA="${PROJECT_SOURCE_DIR}/tests/data/image"
        RUMMAGE_ENGINE_DATA="${PROJECT_SOURCE_DIR}/data/engines")
    add_dependencies(rummage-media-tests rummage-media rummage-preview rummage-fake-preview)
    add_test(NAME media-tests COMMAND rummage-media-tests)
    set_tests_properties(media-tests PROPERTIES TIMEOUT 60)
endif()

if(RUMMAGE_PDF)
    add_executable(rummage-pdf-tests pdf_test.cpp)
    target_link_libraries(rummage-pdf-tests PRIVATE doctest::doctest rummage::isolate
        rummage::thumbs rummage::preview rummage::warnings)
    target_compile_definitions(rummage-pdf-tests PRIVATE
        RUMMAGE_PDF_HELPER="$<TARGET_FILE:rummage-pdf>"
        RUMMAGE_BASE_HELPER="$<TARGET_FILE:rummage-preview>"
        RUMMAGE_PDF_DATA="${PROJECT_SOURCE_DIR}/tests/data/pdf"
        RUMMAGE_ENGINE_DATA="${PROJECT_SOURCE_DIR}/data/engines")
    add_dependencies(rummage-pdf-tests rummage-pdf rummage-preview)
    add_test(NAME pdf-tests COMMAND rummage-pdf-tests)
    # Alone: one case keeps a helper busy until its 8 s timeout, which would slow the timing-
    # sensitive view tests running beside it.
    set_tests_properties(pdf-tests PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()

if(TOKEI_EXECUTABLE)
    target_compile_definitions(rummage-tests PRIVATE RUMMAGE_TEST_TOKEI="${TOKEI_EXECUTABLE}")
endif()

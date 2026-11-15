# Native packages use a private renderer, compiled with their own toolchain.
include(ExternalProject)
set(RUMMAGE_PDFIUM_JOBS 2 CACHE STRING "Parallel jobs for the private PDFium build")
set(_pdfium_sources "${PROJECT_SOURCE_DIR}/third_party/pdfium-sources")
if(NOT EXISTS "${_pdfium_sources}/versions.json")
    message(FATAL_ERROR "Private PDFium sources are missing. Use scripts/ci/export-source.sh to create the complete source archive.")
endif()
set(_pdfium_prefix "${PROJECT_BINARY_DIR}/pdfium-prefix")
file(MAKE_DIRECTORY "${_pdfium_prefix}/include/pdfium")
ExternalProject_Add(rummage-private-pdfium
    SOURCE_DIR "${_pdfium_sources}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND bash "${PROJECT_SOURCE_DIR}/packaging/pdfium/build.sh"
        "${_pdfium_sources}" "${PROJECT_BINARY_DIR}/pdfium-build"
        "${_pdfium_prefix}" "${RUMMAGE_PDFIUM_JOBS}"
    BUILD_BYPRODUCTS "${_pdfium_prefix}/lib/libpdfium.so"
    INSTALL_COMMAND "")
add_library(pdfium SHARED IMPORTED)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${_pdfium_prefix}/lib/libpdfium.so"
    INTERFACE_INCLUDE_DIRECTORIES "${_pdfium_prefix}/include/pdfium")
add_dependencies(pdfium rummage-private-pdfium)
install(FILES "${_pdfium_prefix}/lib/libpdfium.so"
    DESTINATION libexec/rummage COMPONENT pdf)
install(DIRECTORY "${_pdfium_prefix}/share/licenses/rummage-pdf/"
    DESTINATION share/licenses/rummage-pdf COMPONENT pdf)

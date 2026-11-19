#pragma once

#include "rummage/theme/palette.hpp"

namespace rummage::theme {

// The active, materialized KDE palette; no scheme lookup or KConfig inheritance is performed.
// Reads only active View, Window, Button and Selection roles. Normal text is shared by
// Rummage's surfaces, so palettes with incompatible foreground requirements are rejected.
[[nodiscard]] std::filesystem::path default_kde_colors_path();
[[nodiscard]] PaletteResult parse_kde_palette(std::string_view text, std::string_view source);
[[nodiscard]] PaletteResult load_kde_palette(const std::filesystem::path& path);

}  // namespace rummage::theme

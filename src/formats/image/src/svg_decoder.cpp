#include "svg_decoder.hpp"

#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <cstring>
#include <librsvg/rsvg.h>
#include <memory>

#include "rummage/formats/image/svg.hpp"

namespace rummage::formats::image::detail {
namespace {
Loaded failed(GError* error, const char* fallback) {
    Loaded out;
    out.failure = Failure::damaged;
    out.detail = error ? error->message : fallback;
    g_clear_error(&error);
    return out;
}
}  // namespace

Loaded decode_svg(std::span<const std::uint8_t> bytes, std::uint32_t max_width,
                  std::uint32_t max_height) {
    prepare_svg_fonts();
    GError* error = nullptr;
    // Deliberately no base URI: librsvg may resolve internal fragments and data URIs,
    // but cannot resolve file or network resources. Keep its default parsing limits.
    const std::unique_ptr<RsvgHandle, decltype(&g_object_unref)> document(
        rsvg_handle_new_from_data(bytes.data(), bytes.size(), &error), g_object_unref);
    if (!document) {
        return failed(error, "invalid SVG");
    }
    double width = 0;
    double height = 0;
    if (!rsvg_handle_get_intrinsic_size_in_pixels(document.get(), &width, &height)) {
        gboolean has_viewbox = false;
        RsvgRectangle viewbox{};
        rsvg_handle_get_intrinsic_dimensions(document.get(), nullptr, nullptr, nullptr, nullptr,
                                             &has_viewbox, &viewbox);
        width = has_viewbox ? viewbox.width : 256.0;
        height = has_viewbox ? viewbox.height : 256.0;
    }
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
        return failed(nullptr, "invalid SVG dimensions");
    }
    // Normalize before scaling so enormous or tiny finite dimensions cannot overflow.
    const double longest = std::max(width, height);
    const double w = width / longest;
    const double h = height / longest;
    const double scale = std::min(max_width / w, max_height / h);
    const auto out_width = static_cast<std::uint32_t>(
        std::clamp(std::round(w * scale), 1.0, static_cast<double>(max_width)));
    const auto out_height = static_cast<std::uint32_t>(
        std::clamp(std::round(h * scale), 1.0, static_cast<double>(max_height)));
    const std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)> surface(
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(out_width),
                                   static_cast<int>(out_height)),
        cairo_surface_destroy);
    if (cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS) {
        return failed(nullptr, "cannot allocate SVG surface");
    }
    const std::unique_ptr<cairo_t, decltype(&cairo_destroy)> context(cairo_create(surface.get()),
                                                                     cairo_destroy);
    const RsvgRectangle viewport{0, 0, static_cast<double>(out_width),
                                 static_cast<double>(out_height)};
    if (!rsvg_handle_render_document(document.get(), context.get(), &viewport, &error)) {
        return failed(error, "SVG rendering failed");
    }
    if (cairo_status(context.get()) != CAIRO_STATUS_SUCCESS) {
        return failed(nullptr, cairo_status_to_string(cairo_status(context.get())));
    }
    cairo_surface_flush(surface.get());
    const auto* data = cairo_image_surface_get_data(surface.get());
    const auto stride = static_cast<std::size_t>(cairo_image_surface_get_stride(surface.get()));
    Loaded out;
    out.image.width = out_width;
    out.image.height = out_height;
    out.image.rgba.resize(std::size_t{out_width} * out_height * 4);
    for (std::uint32_t y = 0; y < out_height; ++y) {
        for (std::uint32_t x = 0; x < out_width; ++x) {
            // Cairo ARGB32 is native-endian and premultiplied; Image is straight RGBA.
            std::uint32_t pixel = 0;
            std::memcpy(&pixel, data + y * stride + std::size_t{x} * 4, sizeof(pixel));
            const unsigned alpha = pixel >> 24U;
            const auto at = (std::size_t{y} * out_width + x) * 4;
            for (unsigned c = 0; c < 3; ++c) {
                const unsigned value = (pixel >> (16U - c * 8U)) & 255U;
                out.image.rgba[at + c] = static_cast<std::uint8_t>(
                    alpha ? std::min(255U, (value * 255U + alpha / 2U) / alpha) : 0);
            }
            out.image.rgba[at + 3] = static_cast<std::uint8_t>(alpha);
        }
    }
    return out;
}
}  // namespace rummage::formats::image::detail

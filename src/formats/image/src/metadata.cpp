#include "metadata.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <libheif/heif.h>
#include <memory>
#include <set>
#include <span>
#include <string_view>
#include <webp/decode.h>
#include <zlib.h>

#include "jpeg_segments.hpp"
#include "tiff/container.hpp"

namespace rummage::formats::image::detail {
namespace {
using Bytes = std::span<const std::uint8_t>;
constexpr std::size_t kReadLimit = 1024 * 1024;

bool starts(Bytes bytes, std::string_view value) {
    return bytes.size() >= value.size() &&
           std::equal(value.begin(), value.end(), bytes.begin(),
                      [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; });
}

std::uint32_t be32(Bytes b) {
    return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) |
           b[3];
}

std::string string(Bytes b) { return {b.begin(), b.end()}; }

std::string latin1(Bytes b) {
    std::string out;
    for (const auto c : b) {
        if (c >= 128) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 63));
        } else
            out += static_cast<char>(c);
    }
    return out;
}

std::string hex(Bytes b) {
    std::string out;
    for (const auto c : b.first(std::min(b.size(), std::size_t{64})))
        out += std::format("{:02X} ", c);
    if (b.size() > 64)
        out += "…";
    return out;
}

std::string tag_name(std::uint16_t tag) {
    switch (tag) {
        case 0x010E:
            return "Description";
        case 0x010F:
            return "Make";
        case 0x0110:
            return "Model";
        case 0x0112:
            return "Orientation";
        case 0x011A:
            return "X resolution";
        case 0x011B:
            return "Y resolution";
        case 0x0128:
            return "Resolution unit";
        case 0x0131:
            return "Software";
        case 0x0132:
            return "Date/time";
        case 0x013B:
            return "Artist";
        case 0x8298:
            return "Copyright";
        case 0x9003:
            return "Date/time original";
        case 0x9011:
            return "Original time zone";
        case 0x9286:
            return "User comment";
        case 0xA001:
            return "Color space";
        case 0xA430:
            return "Owner";
        case 0xA431:
            return "Camera serial number";
        case 0xA432:
            return "Lens specification";
        case 0xA433:
            return "Lens make";
        case 0xA434:
            return "Lens model";
        case 0xA435:
            return "Lens serial number";
        default:
            return std::format("Tag 0x{:04X}", tag);
    }
}

void tiff_details(const tiff::Source& source, picture::PhotoInfo& info, bool technical) {
    const auto container = tiff::Container::open(source, true);
    if (!container)
        return;
    std::vector<std::pair<std::uint64_t, std::string>> pending{{container->first_ifd(), "IFD0"}};
    std::set<std::uint64_t> visited;
    for (std::size_t index = 0; index < pending.size() && index < 16; ++index) {
        const auto [offset, group] = pending[index];
        if (!visited.insert(offset).second)
            continue;
        const auto ifd = container->ifd(offset);
        if (!ifd)
            continue;
        if (index == 0 && technical) {
            const auto bits = ifd->numbers(tiff::tag::kBitsPerSample, 16);
            std::string depth;
            for (auto bit : bits)
                depth += (depth.empty() ? "" : " / ") + std::to_string(bit);
            info.add_detail("Bit depth", depth.empty() ? "" : depth + " bits");
            if (const auto color = ifd->number(tiff::tag::kPhotometric)) {
                const std::array names{"White is zero", "Grayscale", "RGB",  "Indexed",
                                       "Mask",          "CMYK",      "YCbCr"};
                info.add_detail("Color model",
                                *color < names.size() ? names[*color] : std::to_string(*color));
            }
            if (const auto compression = ifd->number(tiff::tag::kCompression)) {
                std::string name;
                std::string mode;
                switch (*compression) {
                    case 1:
                        name = "None";
                        mode = "Uncompressed";
                        break;
                    case 5:
                        name = "LZW";
                        mode = "Lossless";
                        break;
                    case 7:
                        name = "JPEG";
                        break;
                    case 8:
                    case 32946:
                        name = "Deflate";
                        mode = "Lossless";
                        break;
                    case 32773:
                        name = "PackBits";
                        mode = "Lossless";
                        break;
                    default:
                        name = std::to_string(*compression);
                        break;
                }
                info.add_detail("Compression", name);
                info.add_detail("Compression mode", mode);
            }
            info.add_detail("Byte order",
                            container->little_endian() ? "Little endian" : "Big endian");
        }
        for (const auto& entry : ifd->entries) {
            if (info.details.size() >= picture::PhotoInfo::max_details) {
                info.details_truncated = true;
                return;
            }
            std::string value;
            if (entry.type == static_cast<unsigned>(tiff::Type::ascii)) {
                const auto bytes = ifd->bytes(entry.tag, picture::PhotoInfo::max_detail_bytes);
                value = latin1(bytes);
                while (!value.empty() && value.back() == '\0')
                    value.pop_back();
            } else if (entry.type != static_cast<unsigned>(tiff::Type::undefined)) {
                for (const auto number : ifd->reals(entry.tag, 64))
                    value += (value.empty() ? "" : ", ") + std::format("{:.12g}", number);
            }
            if (value.empty()) {
                value = std::format("{} values, type {}", entry.count, entry.type);
                const auto bytes = ifd->bytes(entry.tag, 64);
                if (!bytes.empty())
                    value += ": " + hex(bytes);
                else if (entry.count != 0)
                    value += " (data omitted)";
            }
            info.add_detail(group + " · " + tag_name(entry.tag), value);
            if (entry.tag == 0x8769 || entry.tag == 0x8825 || entry.tag == 0xA005 ||
                entry.tag == tiff::tag::kSubIfds) {
                const auto children = ifd->numbers(entry.tag, 16);
                if (children.empty() && entry.count)
                    info.details_truncated = true;
                for (const auto child : children) {
                    if (pending.size() >= 16) {
                        info.details_truncated = true;
                        break;
                    }
                    pending.emplace_back(child, entry.tag == 0x8769   ? "EXIF"
                                                : entry.tag == 0x8825 ? "GPS"
                                                                      : "SubIFD");
                }
            }
        }
        if (ifd->next && pending.size() < 16)
            pending.emplace_back(ifd->next, "IFD" + std::to_string(index + 1));
        else if (ifd->next)
            info.details_truncated = true;
    }
}

std::vector<std::uint8_t> inflate_text(Bytes bytes, picture::PhotoInfo& info) {
    std::vector<std::uint8_t> out(picture::PhotoInfo::max_detail_bytes);
    uLongf size = out.size();
    const int result = uncompress(out.data(), &size, bytes.data(), bytes.size());
    if (result == Z_BUF_ERROR)
        info.details_truncated = true;
    if (result != Z_OK && result != Z_BUF_ERROR)
        return {};
    out.resize(size);
    return out;
}

void png_details(const FdFile& file, picture::PhotoInfo& info) {
    std::uint64_t at = 8;
    std::size_t budget = kReadLimit;
    for (unsigned chunks = 0; chunks < 4096 && at < file.size(); ++chunks) {
        const auto header = file.read_exactly(at, 8);
        if (header.empty())
            return;
        const auto size = be32(header);
        const auto type = string(Bytes{header}.subspan(4));
        if (file.size() - at < 12 || size > file.size() - at - 12)
            return;
        if (type == "IEND")
            return;
        if (type == "IDAT" || type == "fdAT") {
            at += std::uint64_t{size} + 12;
            continue;
        }
        if (size > budget) {
            info.add_detail("PNG · " + type, std::format("{} bytes (data omitted)", size));
            info.details_truncated = true;
            at += std::uint64_t{size} + 12;
            continue;
        }
        budget -= size;
        const auto storage = file.read_exactly(at + 8, static_cast<std::size_t>(size) + 4);
        if (storage.empty())
            return;
        const Bytes data{storage.data(), size};
        auto crc = crc32(0, header.data() + 4, 4);
        crc = crc32(crc, data.data(), static_cast<uInt>(data.size()));
        if (crc != be32(Bytes{storage}.subspan(size))) {
            info.add_detail("Metadata error", "Invalid PNG chunk checksum: " + type);
            return;
        }
        if (type == "IHDR" && size == 13) {
            const auto depth = data[8], color_type = data[9];
            const bool valid_depth =
                color_type == 0
                    ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                : color_type == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                                  : (color_type == 2 || color_type == 4 || color_type == 6) &&
                                        (depth == 8 || depth == 16);
            if (!valid_depth || !be32(data) || !be32(data.subspan(4)) || data[10] != 0 ||
                data[11] != 0 || data[12] > 1) {
                info.add_detail("Metadata error", "Invalid PNG image header");
                return;
            }
            info.width = be32(data);
            info.height = be32(data.subspan(4));
            info.add_detail("Format", "PNG");
            const auto color = data[9];
            info.add_detail("Color model", color == 0   ? "Grayscale"
                                           : color == 2 ? "RGB"
                                           : color == 3 ? "Indexed"
                                           : color == 4 ? "Grayscale + alpha"
                                           : color == 6 ? "RGBA"
                                                        : "Unknown");
            info.add_detail("Bit depth", std::format("{} bits", data[8]));
            info.add_detail("Compression", data[10] == 0 ? "Deflate" : "Unknown");
            info.add_detail("Compression mode", "Lossless");
            info.add_detail("Interlacing", data[12] == 0   ? "None"
                                           : data[12] == 1 ? "Adam7"
                                                           : "Unknown");
            info.add_detail("Filter method", std::to_string(data[11]));
        } else if (type == "tEXt" || type == "zTXt" || type == "iTXt") {
            const auto zero = std::ranges::find(data, 0);
            const auto key_size = static_cast<std::size_t>(zero - data.begin());
            if (zero == data.end() || key_size == 0 || key_size > 79) {
                at += std::uint64_t{size} + 12;
                continue;
            }
            const auto key = latin1(data.first(key_size));
            auto text = data.subspan(key_size + 1);
            std::vector<std::uint8_t> expanded;
            if (type == "zTXt") {
                if (text.empty() || text[0] != 0)
                    return;
                expanded = inflate_text(text.subspan(1), info);
                text = expanded;
            } else if (type == "iTXt") {
                if (text.size() < 2 || text[0] > 1 || text[1] != 0)
                    return;
                const bool compressed = text[0] == 1;
                text = text.subspan(2);
                for (int field = 0; field < 2; ++field) {
                    const auto end = std::ranges::find(text, 0);
                    if (end == text.end())
                        return;
                    text = text.subspan(static_cast<std::size_t>(end - text.begin()) + 1);
                }
                if (compressed) {
                    expanded = inflate_text(text, info);
                    text = expanded;
                }
            }
            info.add_detail("PNG · " + key, type == "iTXt" ? string(text) : latin1(text));
        } else if (type == "gAMA" && size == 4)
            info.add_detail("Gamma", std::format("{:.5f}", be32(data) / 100000.0));
        else if (type == "sRGB" && size == 1) {
            info.add_detail("Color profile", "sRGB");
            const std::array intents{"Perceptual", "Relative colorimetric", "Saturation",
                                     "Absolute colorimetric"};
            info.add_detail("Rendering intent",
                            data[0] < intents.size() ? intents[data[0]] : "Unknown");
        } else if (type == "iCCP") {
            const auto zero = std::ranges::find(data, 0);
            info.add_detail("ICC profile",
                            latin1(data.first(static_cast<std::size_t>(zero - data.begin()))));
        } else if (type == "pHYs" && size == 9)
            info.add_detail("Pixel density",
                            std::format("{} × {} {}", be32(data), be32(data.subspan(4)),
                                        data[8] == 1 ? "pixels/metre" : "(unit unspecified)"));
        else if (type == "tIME" && size == 7)
            info.add_detail("Image modified",
                            std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
                                        (unsigned{data[0]} << 8) | data[1], data[2], data[3],
                                        data[4], data[5], data[6]));
        else if (type == "acTL" && size == 8) {
            info.add_detail("Frames", std::to_string(be32(data)));
            info.add_detail("Loop count", be32(data.subspan(4)) == 0
                                              ? "Infinite"
                                              : std::to_string(be32(data.subspan(4))));
        } else if (type == "eXIf")
            tiff_details(tiff::Source{data}, info, false);
        else if (type == "tRNS")
            info.add_detail("Transparency", "Yes (transparency chunk)");
        else
            info.add_detail("PNG · " + type, std::format("{} bytes: {}", size, hex(data)));
        at += std::uint64_t{size} + 12;
        if (info.details.size() >= picture::PhotoInfo::max_details) {
            info.details_truncated = true;
            return;
        }
    }
    if (at < file.size())
        info.details_truncated = true;
}

void jpeg_details(Bytes head, picture::PhotoInfo& info) {
    info.add_detail("Format", "JPEG");
    std::size_t at = 2;
    while (at + 4 <= head.size() && head[at] == 0xFF) {
        const auto marker = head[at + 1];
        if (marker == 0xFF) {
            ++at;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9)
            return;
        const std::size_t size = (std::size_t{head[at + 2]} << 8) | head[at + 3];
        if (size < 2 || size > head.size() - at - 2)
            break;
        const auto data = head.subspan(at + 4, size - 2);
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC && data.size() >= 6) {
            info.add_detail("Bit depth", std::format("{} bits", data[0]));
            info.add_detail("Components", std::to_string(data[5]));
            const bool lossless =
                marker == 0xC3 || marker == 0xC7 || marker == 0xCB || marker == 0xCF;
            info.add_detail("Compression mode", lossless ? "Lossless" : "Lossy");
            info.add_detail("Encoding", marker == 0xC0   ? "Baseline DCT"
                                        : marker == 0xC2 ? "Progressive DCT"
                                                         : std::format("SOF {:X}", marker - 0xC0));
            std::string sampling;
            for (std::size_t i = 0; i < data[5] && 6 + i * 3 + 2 < data.size(); ++i) {
                const auto sample = data[7 + i * 3];
                sampling +=
                    (sampling.empty() ? "" : ", ") + std::format("{}×{}", sample >> 4, sample & 15);
            }
            info.add_detail("Component sampling", sampling);
        } else if (marker == 0xFE)
            info.add_detail("JPEG · Comment", latin1(data));
        else if (marker == 0xE1 && starts(data, std::string_view{"Exif\0\0", 6}))
            tiff_details(tiff::Source{data.subspan(6)}, info, false);
        else if (marker == 0xE1 && starts(data, "http://ns.adobe.com/xap/1.0/")) {
            const auto zero = std::ranges::find(data, 0);
            if (zero != data.end())
                info.add_detail(
                    "XMP", string(data.subspan(static_cast<std::size_t>(zero - data.begin()) + 1)));
        } else if (marker == 0xE2 && starts(data, "ICC_PROFILE"))
            info.add_detail("ICC profile",
                            std::format("Embedded ({} bytes in this segment)", data.size()));
        else if (marker == 0xED)
            info.add_detail("IPTC / Photoshop",
                            std::format("{} bytes: {}", data.size(), hex(data)));
        at += size + 2;
    }
    info.details_truncated = true;
}

void heif_details(const FdFile& file, picture::PhotoInfo& info) {
    struct Reader {
        const FdFile& file;
        std::uint64_t position{0};
        std::size_t budget{8 * kReadLimit};
    } source{file};

    heif_reader reader{};
    reader.reader_api_version = 1;
    reader.get_position = [](void* user) -> std::int64_t {
        return static_cast<std::int64_t>(static_cast<Reader*>(user)->position);
    };
    reader.read = [](void* data, std::size_t size, void* user) -> int {
        auto& state = *static_cast<Reader*>(user);
        if (size > state.budget)
            return 1;
        state.budget -= size;
        const auto got =
            state.file.read_into(state.position, {static_cast<std::uint8_t*>(data), size});
        if (!got || *got != size)
            return 1;
        state.position += size;
        return 0;
    };
    reader.seek = [](std::int64_t position, void* user) -> int {
        auto& state = *static_cast<Reader*>(user);
        if (position < 0 || static_cast<std::uint64_t>(position) > state.file.size())
            return 1;
        state.position = static_cast<std::uint64_t>(position);
        return 0;
    };
    reader.wait_for_file_size = [](std::int64_t size, void* user) {
        return size >= 0 &&
                       static_cast<std::uint64_t>(size) <= static_cast<Reader*>(user)->file.size()
                   ? heif_reader_grow_status_size_reached
                   : heif_reader_grow_status_size_beyond_eof;
    };
    const std::unique_ptr<heif_context, decltype(&heif_context_free)> context{heif_context_alloc(),
                                                                              heif_context_free};
    if (!context || heif_context_read_from_reader(context.get(), &reader, &source, nullptr).code !=
                        heif_error_Ok)
        return;
    heif_image_handle* raw = nullptr;
    if (heif_context_get_primary_image_handle(context.get(), &raw).code != heif_error_Ok)
        return;
    const std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)> handle{
        raw, heif_image_handle_release};
    info.width = static_cast<std::uint32_t>(std::max(0, heif_image_handle_get_width(raw)));
    info.height = static_cast<std::uint32_t>(std::max(0, heif_image_handle_get_height(raw)));
    info.add_detail("Format", "HEIF");
    const int bits = heif_image_handle_get_luma_bits_per_pixel(raw);
    if (bits > 0)
        info.add_detail("Bit depth", std::format("{} bits", bits));
    info.add_detail("Alpha", heif_image_handle_has_alpha_channel(raw) ? "Yes" : "No");
    info.add_detail("Images",
                    std::to_string(heif_context_get_number_of_top_level_images(context.get())));
    heif_color_profile_nclx* profile = nullptr;
    if (heif_image_handle_get_nclx_color_profile(raw, &profile).code == heif_error_Ok && profile) {
        info.add_detail("Color primaries", std::to_string(profile->color_primaries));
        info.add_detail("Transfer characteristics",
                        std::to_string(profile->transfer_characteristics));
        info.add_detail("Matrix coefficients", std::to_string(profile->matrix_coefficients));
        info.add_detail("Color range", profile->full_range_flag ? "Full" : "Limited");
        heif_nclx_color_profile_free(profile);
    }
    const auto profile_size = heif_image_handle_get_raw_color_profile_size(raw);
    if (profile_size)
        info.add_detail("ICC profile", std::format("Embedded ({} bytes)", profile_size));
    std::array<heif_item_id, 64> ids{};
    if (heif_image_handle_get_number_of_metadata_blocks(raw, nullptr) >
        static_cast<int>(ids.size()))
        info.details_truncated = true;
    const int count = heif_image_handle_get_list_of_metadata_block_IDs(
        raw, nullptr, ids.data(), static_cast<int>(ids.size()));
    std::size_t budget = kReadLimit;
    for (int i = 0; i < count; ++i) {
        const auto id = ids[static_cast<std::size_t>(i)];
        const auto size = heif_image_handle_get_metadata_size(raw, id);
        const char* type = heif_image_handle_get_metadata_type(raw, id);
        if (!type)
            continue;
        if (size > budget) {
            info.add_detail(std::string{"HEIF · "} + type,
                            std::format("{} bytes (data omitted)", size));
            info.details_truncated = true;
            continue;
        }
        budget -= size;
        std::vector<std::uint8_t> data(size);
        if (heif_image_handle_get_metadata(raw, id, data.data()).code != heif_error_Ok)
            continue;
        if (std::string_view{type} == "Exif" && data.size() >= 4) {
            const auto offset = be32(data);
            if (offset <= data.size() - 4)
                tiff_details(tiff::Source{Bytes{data}.subspan(4 + offset)}, info, false);
        } else {
            const char* content = heif_image_handle_get_metadata_content_type(raw, id);
            const bool textual =
                content && (std::string_view{content}.find("xml") != std::string_view::npos ||
                            std::string_view{content}.starts_with("text/"));
            info.add_detail(std::string{"HEIF · "} + type, textual ? string(data) : hex(data));
        }
    }
}

}  // namespace

void read_metadata(const FdFile& file, picture::PhotoInfo& info) {
    const auto head = file.read(0, kReadLimit);
    const Bytes bytes{head};
    if (starts(bytes, "\x89PNG\r\n\x1a\n")) {
        info.width = info.height = 0;
        png_details(file, info);
    } else if (starts(bytes, "\xFF\xD8"))
        jpeg_details(bytes, info);
    else if (starts(bytes, "II") || starts(bytes, "MM")) {
        info.add_detail("Format", "TIFF / camera raw");
        tiff_details(tiff::Source{file}, info, true);
    } else if (starts(bytes, "GIF87a") || starts(bytes, "GIF89a")) {
        if (bytes.size() < 13)
            return;
        info.add_detail("Format", string(bytes.first(6)));
        info.add_detail("Color model", "Indexed RGB");
        info.add_detail("Compression", "LZW");
        info.add_detail("Compression mode", "Lossless");
        info.add_detail("Color resolution", std::format("{} bits", ((bytes[10] >> 4) & 7) + 1));
        if (bytes[10] & 128)
            info.add_detail("Global palette entries", std::to_string(1U << ((bytes[10] & 7) + 1)));
    } else if (starts(bytes, "RIFF") && bytes.size() >= 12 && starts(bytes.subspan(8), "WEBP")) {
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(bytes.data(), bytes.size(), &features) != VP8_STATUS_OK)
            return;
        info.add_detail("Format", "WebP");
        info.add_detail("Bit depth", "8 bits");
        info.add_detail("Color model", features.has_alpha ? "RGBA" : "RGB");
        info.add_detail("Compression mode", features.format == 1   ? "Lossy"
                                            : features.format == 2 ? "Lossless"
                                                                   : "Mixed / animated");
        info.add_detail("Animation", features.has_animation ? "Yes" : "No");
    } else if (bytes.size() >= 12 && starts(bytes.subspan(4), "ftyp")) {
        heif_details(file, info);
    } else if (JxlSignatureCheck(bytes.data(), bytes.size()) == JXL_SIG_CODESTREAM ||
               JxlSignatureCheck(bytes.data(), bytes.size()) == JXL_SIG_CONTAINER) {
        const auto decoder = JxlDecoderMake(nullptr);
        if (!decoder ||
            JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS ||
            JxlDecoderSetInput(decoder.get(), bytes.data(), bytes.size()) != JXL_DEC_SUCCESS)
            return;
        if (JxlDecoderProcessInput(decoder.get()) != JXL_DEC_BASIC_INFO)
            return;
        JxlBasicInfo basic{};
        if (JxlDecoderGetBasicInfo(decoder.get(), &basic) != JXL_DEC_SUCCESS)
            return;
        info.width = basic.xsize;
        info.height = basic.ysize;
        info.add_detail("Format", "JPEG XL");
        info.add_detail("Bit depth", std::format("{} bits", basic.bits_per_sample));
        info.add_detail("Color channels", std::to_string(basic.num_color_channels));
        info.add_detail("Extra channels", std::to_string(basic.num_extra_channels));
        info.add_detail("Alpha depth", std::format("{} bits", basic.alpha_bits));
        info.add_detail("Animation", basic.have_animation ? "Yes" : "No");
        info.add_detail("Intensity target", std::format("{} nits", basic.intensity_target));
    }
}
}  // namespace rummage::formats::image::detail

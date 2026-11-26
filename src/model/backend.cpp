#include "backend.hpp"

#include <algorithm>
#include <array>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cerrno>
#include <cmath>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace rummage::isolate::backend {
namespace {
constexpr std::size_t kInputLimit = 32 * 1024 * 1024;
constexpr std::size_t kTriangleLimit = 100000;
constexpr std::uint64_t kRasterLimit = 64000000;

// ReadFileFromMemory wraps this with its memory stream. Ancillary resources, including
// network/file glTF buffers and OBJ materials, must never reach a filesystem backend.
class ClosedIO final : public Assimp::IOSystem {
public:
    bool Exists(const char*) const override { return false; }

    char getOsSeparator() const override { return '/'; }

    Assimp::IOStream* Open(const char*, const char* = "rb") override { return nullptr; }

    void Close(Assimp::IOStream*) override {}
};

picture::Loaded failed(picture::Failure reason, std::string detail) {
    picture::Loaded result;
    result.failure = reason;
    result.detail = std::move(detail);
    return result;
}

struct Point {
    double x{}, y{}, z{};
};

using Triangle = std::array<Point, 3>;

bool finite(const Point& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

double edge(const Point& a, const Point& b, double x, double y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}
}  // namespace

void warm_up() {}

picture::Loaded make_picture(int fd, std::uint32_t width, std::uint32_t height,
                             std::uint32_t page) {
    if (page != 0)
        return failed(picture::Failure::unknown_format, "models have one geometry preview");
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return failed(picture::Failure::unreadable, "model is not a readable regular file");
    if (static_cast<std::uint64_t>(st.st_size) > kInputLimit)
        return failed(picture::Failure::too_large, "model exceeds 32 MiB input limit");
    std::vector<char> bytes(static_cast<std::size_t>(st.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto got =
            ::pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return failed(picture::Failure::unreadable, "model read failed");
        offset += static_cast<std::size_t>(got);
    }
    Assimp::Importer importer;
    importer.SetIOHandler(new ClosedIO);
    const auto byte = [&](std::size_t at) {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at]));
    };
    const char* hint = "";
    if (bytes.size() >= 84) {
        const std::uint32_t count =
            byte(80) | (byte(81) << 8U) | (byte(82) << 16U) | (byte(83) << 24U);
        if (84ULL + 50ULL * count == bytes.size())
            hint = "stl";
    }
    if (bytes.size() >= 4 && std::string_view{bytes.data(), 4} == "glTF")
        hint = "glb";
    const auto* scene =
        importer.ReadFileFromMemory(bytes.data(), bytes.size(),
                                    aiProcess_Triangulate | aiProcess_PreTransformVertices |
                                        aiProcess_ValidateDataStructure | aiProcess_SortByPType,
                                    hint);
    if (!scene || !scene->HasMeshes())
        return failed(picture::Failure::damaged, "model has no readable standalone geometry");
    std::vector<Triangle> triangles;
    Point low{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
              std::numeric_limits<double>::infinity()};
    Point high{-low.x, -low.y, -low.z};
    for (unsigned m = 0; m < scene->mNumMeshes; ++m) {
        const auto& mesh = *scene->mMeshes[m];
        for (unsigned f = 0; f < mesh.mNumFaces; ++f) {
            const auto& face = mesh.mFaces[f];
            if (face.mNumIndices != 3)
                continue;
            if (triangles.size() == kTriangleLimit)
                return failed(picture::Failure::too_large, "model exceeds 100000 triangle limit");
            Triangle triangle;
            for (unsigned c = 0; c < 3; ++c) {
                if (face.mIndices[c] >= mesh.mNumVertices)
                    return failed(picture::Failure::damaged, "model index is outside mesh");
                const auto& v = mesh.mVertices[face.mIndices[c]];
                Point p{static_cast<double>(v.x), static_cast<double>(v.y),
                        static_cast<double>(v.z)};
                if (!finite(p))
                    return failed(picture::Failure::damaged, "non-finite model coordinates");
                // Fixed isometric camera, with no scene scripts, lights, textures or animation.
                p = {(p.x - p.z) * 0.7071067811865476,
                     (p.x + p.z) * 0.408248290463863 - p.y * 0.816496580927726,
                     (p.x + p.y + p.z) * 0.5773502691896258};
                triangle[c] = p;
                low.x = std::min(low.x, p.x);
                low.y = std::min(low.y, p.y);
                low.z = std::min(low.z, p.z);
                high.x = std::max(high.x, p.x);
                high.y = std::max(high.y, p.y);
                high.z = std::max(high.z, p.z);
            }
            triangles.push_back(triangle);
        }
    }
    if (triangles.empty() || !(high.x > low.x || high.y > low.y))
        return failed(picture::Failure::no_readable_preview, "model has no visible triangles");
    width = std::clamp(width, 1U, 1024U);
    height = std::clamp(height, 1U, 1024U);
    const double scale =
        0.85 * std::min(static_cast<double>(width) / std::max(high.x - low.x, 1e-30),
                        static_cast<double>(height) / std::max(high.y - low.y, 1e-30));
    const Point center{(low.x + high.x) / 2, (low.y + high.y) / 2, (low.z + high.z) / 2};
    picture::Loaded result;
    result.image = {
        .width = width,
        .height = height,
        .rgba = std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4, 255)};
    std::vector<double> depth(static_cast<std::size_t>(width) * height,
                              -std::numeric_limits<double>::infinity());
    std::uint64_t work = 0;
    bool ink = false;
    for (auto triangle : triangles) {
        for (auto& p : triangle) {
            p.x = (p.x - center.x) * scale + static_cast<double>(width) / 2;
            p.y = (p.y - center.y) * scale + static_cast<double>(height) / 2;
            p.z = (p.z - center.z) * scale;
        }
        const auto& a = triangle[0];
        const auto& b = triangle[1];
        const auto& c = triangle[2];
        const double area = edge(a, b, c.x, c.y);
        if (std::abs(area) < 1e-12)
            continue;
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
        const int x1 = std::min(static_cast<int>(width) - 1,
                                static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
        const int y1 = std::min(static_cast<int>(height) - 1,
                                static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));
        if (x1 < x0 || y1 < y0)
            continue;
        work += static_cast<std::uint64_t>(x1 - x0 + 1) * static_cast<std::uint64_t>(y1 - y0 + 1);
        if (work > kRasterLimit)
            return failed(picture::Failure::too_large, "model exceeds bounded raster work");
        const double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        const double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        const double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const double light =
            length > 0 ? 0.35 + 0.65 * std::abs((nx * 0.3 - ny * 0.4 + nz * 0.8660254) / length)
                       : 0.35;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double wa = edge(b, c, x + 0.5, y + 0.5) / area;
                const double wb = edge(c, a, x + 0.5, y + 0.5) / area;
                const double wc = 1 - wa - wb;
                if (wa < 0 || wb < 0 || wc < 0)
                    continue;
                const auto index = static_cast<std::size_t>(y) * width + static_cast<unsigned>(x);
                const double z = wa * a.z + wb * b.z + wc * c.z;
                if (z < depth[index])
                    continue;
                depth[index] = z;
                ink = true;
                result.image.rgba[index * 4] = static_cast<std::uint8_t>(70 * light);
                result.image.rgba[index * 4 + 1] = static_cast<std::uint8_t>(150 * light);
                result.image.rgba[index * 4 + 2] = static_cast<std::uint8_t>(220 * light);
            }
    }
    if (!ink)
        return failed(picture::Failure::no_readable_preview, "model has no visible surface");
    return result;
}

picture::PhotoInfo photo_info(int) { return {}; }

text::Loaded read_text(int, const text::Limits&) {
    text::Loaded result;
    result.failure = text::Failure::unreadable;
    return result;
}

void colour(text::Text&, std::string_view, std::string_view) {}
}  // namespace rummage::isolate::backend

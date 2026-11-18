#include <cairo-ft.h>
#include <fontconfig/fcfreetype.h>
#include <fontconfig/fontconfig.h>
#include <fstream>
#include <map>
#include <memory>
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <set>
#include <string>
#include <vector>

#include "rummage/formats/image/svg.hpp"

namespace rummage::formats::image {
namespace {
template <typename T, auto Destroy>
using Owned = std::unique_ptr<T, decltype(Destroy)>;

struct Face {
    FT_Library library{};
    FT_Face face{};
    std::vector<unsigned char> bytes;

    ~Face() {
        if (face) {
            FT_Done_Face(face);
        }
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

const cairo_user_data_key_t kFaceOwner{};

// Pango shapes with HarfBuzz, while Cairo draws with FreeType. Prepare both: a
// warm-up string alone only caches its chosen size/style and can lose its face
// when Cairo evicts a cache entry. FC_FT_FACE supplies a memory-backed face at
// every size, and Pango retains the corresponding HarfBuzz file data in its map.
class Fonts {
public:
    Fonts() {
        Owned<FcConfig, &FcConfigDestroy> system(FcInitLoadConfigAndFonts(), FcConfigDestroy);
        Owned<FcConfig, &FcConfigDestroy> config(FcConfigCreate(), FcConfigDestroy);
        if (!system || !config) {
            return;
        }
        FcConfigSetRescanInterval(config.get(), 0);
        std::map<std::string, std::set<std::string>> files;
        for (const char* family : {"sans-serif", "serif", "monospace"}) {
            for (const int weight : {FC_WEIGHT_REGULAR, FC_WEIGHT_BOLD}) {
                for (const int slant : {FC_SLANT_ROMAN, FC_SLANT_ITALIC}) {
                    Owned<FcPattern, &FcPatternDestroy> wanted(FcPatternCreate(), FcPatternDestroy);
                    FcPatternAddString(wanted.get(), FC_FAMILY,
                                       reinterpret_cast<const FcChar8*>(family));
                    FcPatternAddInteger(wanted.get(), FC_WEIGHT, weight);
                    FcPatternAddInteger(wanted.get(), FC_SLANT, slant);
                    FcConfigSubstitute(system.get(), wanted.get(), FcMatchPattern);
                    FcDefaultSubstitute(wanted.get());
                    FcResult result{};
                    Owned<FcPattern, &FcPatternDestroy> match(
                        FcFontMatch(system.get(), wanted.get(), &result), FcPatternDestroy);
                    FcChar8* file = nullptr;
                    if (match &&
                        FcPatternGetString(match.get(), FC_FILE, 0, &file) == FcResultMatch) {
                        files[reinterpret_cast<const char*>(file)].emplace(family);
                    }
                }
            }
        }
        // At most 12 files and 64 MiB, including collection faces; no document
        // can select a new file. Missing families fall back within this set.
        std::size_t total = 0;
        for (const auto& [file, families] : files) {
            static_cast<void>(families);
            std::ifstream input(file, std::ios::binary | std::ios::ate);
            const auto size = input.tellg();
            if (size <= 0 || size > 64 * 1024 * 1024 ||
                static_cast<std::size_t>(size) > 64U * 1024U * 1024U - total) {
                continue;
            }
            if (FcConfigAppFontAddFile(config.get(),
                                       reinterpret_cast<const FcChar8*>(file.c_str()))) {
                total += static_cast<std::size_t>(size);
            }
        }
        FcFontSet* set = FcConfigGetFonts(config.get(), FcSetApplication);
        if (set) {
            // Keep only faces we successfully loaded. Each retained pattern owns a
            // Cairo font face, whose user data keeps the FreeType bytes alive even
            // if Cairo's scaled-font cache outlives this font map.
            int retained = 0;
            std::size_t bytes = 0;
            for (int i = 0; i < set->nfont; ++i) {
                FcPattern* pattern = set->fonts[i];
                if (retained < 64 && prepare_face(pattern, bytes)) {
                    FcChar8* file = nullptr;
                    if (FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch) {
                        for (const auto& family : files.at(reinterpret_cast<const char*>(file))) {
                            FcPatternAddString(pattern, FC_FAMILY,
                                               reinterpret_cast<const FcChar8*>(family.c_str()));
                        }
                    }
                    set->fonts[retained++] = pattern;
                } else {
                    FcPatternDestroy(pattern);
                }
            }
            set->nfont = retained;
        }
        map_.reset(pango_cairo_font_map_new_for_font_type(CAIRO_FONT_TYPE_FT));
        if (!map_) {
            return;
        }
        auto* fcmap = PANGO_FC_FONT_MAP(map_.get());
        pango_fc_font_map_set_config(fcmap, config.get());
        const Owned<PangoContext, &g_object_unref> context(
            pango_font_map_create_context(map_.get()), g_object_unref);
        if (set) {
            for (int i = 0; i < set->nfont; ++i) {
                Owned<PangoFontDescription, &pango_font_description_free> desc(
                    pango_fc_font_description_from_pattern(set->fonts[i], false),
                    pango_font_description_free);
                pango_font_description_set_size(desc.get(), 12 * PANGO_SCALE);
                const Owned<PangoFont, &g_object_unref> font(
                    pango_font_map_load_font(map_.get(), context.get(), desc.get()),
                    g_object_unref);
                if (font) {
                    static_cast<void>(pango_font_get_hb_font(font.get()));
                }
            }
        }
        pango_cairo_font_map_set_default(PANGO_CAIRO_FONT_MAP(map_.get()));
    }

    ~Fonts() {
        if (map_) {
            pango_cairo_font_map_set_default(nullptr);
            pango_fc_font_map_shutdown(PANGO_FC_FONT_MAP(map_.get()));
        }
    }

private:
    bool prepare_face(FcPattern* pattern, std::size_t& bytes) {
        FcChar8* file = nullptr;
        int index = 0;
        if (FcPatternGetString(pattern, FC_FILE, 0, &file) != FcResultMatch) {
            return false;
        }
        FcPatternGetInteger(pattern, FC_INDEX, 0, &index);
        std::ifstream input(reinterpret_cast<const char*>(file), std::ios::binary | std::ios::ate);
        const auto size = input.tellg();
        if (size <= 0 || size > 64 * 1024 * 1024 ||
            static_cast<std::size_t>(size) > 64U * 1024U * 1024U - bytes) {
            return false;
        }
        auto face = std::make_unique<Face>();
        face->bytes.resize(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(face->bytes.data()), size) ||
            FT_Init_FreeType(&face->library) != 0 ||
            FT_New_Memory_Face(face->library, face->bytes.data(), static_cast<FT_Long>(size), index,
                               &face->face) != 0) {
            return false;
        }
        Owned<cairo_font_face_t, &cairo_font_face_destroy> cairo_face(
            cairo_ft_font_face_create_for_ft_face(face->face, 0), cairo_font_face_destroy);
        if (cairo_font_face_status(cairo_face.get()) != CAIRO_STATUS_SUCCESS ||
            cairo_font_face_set_user_data(cairo_face.get(), &kFaceOwner, face.get(), [](void* p) {
                delete static_cast<Face*>(p);
            }) != CAIRO_STATUS_SUCCESS) {
            return false;
        }
        auto* owned = face.release();
        FcPatternDel(pattern, FC_FT_FACE);
        FcPatternAddFTFace(pattern, FC_FT_FACE, owned->face);
        bytes += static_cast<std::size_t>(size);
        faces_.push_back(std::move(cairo_face));
        return true;
    }

    std::vector<Owned<cairo_font_face_t, &cairo_font_face_destroy>> faces_;
    Owned<PangoFontMap, &g_object_unref> map_{nullptr, g_object_unref};
};
}  // namespace

void prepare_svg_fonts() { thread_local Fonts fonts; }
}  // namespace rummage::formats::image

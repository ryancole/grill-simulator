#pragma once

#include <filesystem>
#include <unordered_map>

// One glyph of an MSDF atlas, straight from msdf-atlas-gen's CSV. The plane
// bounds are the glyph's quad in em units, relative to the pen sitting on the
// baseline; the atlas bounds are its rectangle in the atlas texture, in texels.
//
// The atlas is baked with -yorigin top, so both y axes point down, matching
// Direct3D texture space. The `l/b/r/t` fields are kept exactly as the CSV names
// them: a quad corner pairs plane_b with atlas_b and plane_t with atlas_t, and
// because both sides share the same origin that mapping lands the glyph upright
// with no special-casing of which edge is visually higher.
struct Glyph {
    float advance = 0.0f; // Pen movement after this glyph, in em.
    float plane_l = 0.0f, plane_b = 0.0f, plane_r = 0.0f, plane_t = 0.0f;
    float atlas_l = 0.0f, atlas_b = 0.0f, atlas_r = 0.0f, atlas_t = 0.0f;
    // Whitespace has a real advance but no quad; msdf-atlas-gen emits a degenerate
    // rectangle for it, which this flags so the layout skips drawing it.
    bool visible = false;
};

// A loaded MSDF font: its glyphs keyed by Unicode code point. The atlas texture
// itself is loaded separately by the renderer, from the .png beside the .csv.
struct Font {
    std::unordered_map<char32_t, Glyph> glyphs;

    // The glyph for `code`, or nullptr if the atlas has none (it was baked from
    // the printable-ASCII charset).
    const Glyph* Find(char32_t code) const;
};

// Parses the glyph-metrics CSV produced by `bake_font_atlas`. Throws if the file
// cannot be opened.
Font LoadFontCsv(const std::filesystem::path& csv_path);

// The distance field's width in atlas texels. Must match PXRANGE passed to
// bake_font_atlas in cmake/Fonts.cmake: the pixel shader divides it by the atlas
// dimensions to recover how far, in screen pixels, each glyph edge is antialiased.
inline constexpr float kDistanceRange = 4.0f;

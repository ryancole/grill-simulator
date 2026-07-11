#include "font.hpp"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Splits one CSV line into its comma-separated fields, parsed as floats. The unit
// code point in the first column is a whole number but reads fine as a float.
bool ParseRow(std::string_view line, std::vector<float>& out) {
    out.clear();
    size_t start = 0;
    while (start <= line.size()) {
        const size_t comma = line.find(',', start);
        const size_t end = comma == std::string_view::npos ? line.size() : comma;

        std::string_view field = line.substr(start, end - start);
        // msdf-atlas-gen writes values like "-0"; from_chars handles the sign, but
        // a stray trailing '\r' from a CRLF file would not parse, so trim it.
        while (!field.empty() && (field.back() == '\r' || field.back() == ' ')) {
            field.remove_suffix(1);
        }

        float value = 0.0f;
        const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
        if (ec != std::errc{} || ptr != field.data() + field.size()) {
            return false;
        }
        out.push_back(value);

        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

} // namespace

const Glyph* Font::Find(char32_t code) const {
    const auto it = glyphs.find(code);
    return it == glyphs.end() ? nullptr : &it->second;
}

Font LoadFontCsv(const std::filesystem::path& csv_path) {
    std::ifstream file(csv_path);
    if (!file) {
        throw std::runtime_error("Cannot open font metrics " + csv_path.string());
    }

    Font font;
    std::string line;
    std::vector<float> row;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        // unicode, advance, plane l/b/r/t, atlas l/b/r/t -- ten columns. Anything
        // shorter is not a glyph row this loader understands.
        if (!ParseRow(line, row) || row.size() != 10) {
            continue;
        }

        Glyph glyph{};
        glyph.advance = row[1];
        glyph.plane_l = row[2];
        glyph.plane_b = row[3];
        glyph.plane_r = row[4];
        glyph.plane_t = row[5];
        glyph.atlas_l = row[6];
        glyph.atlas_b = row[7];
        glyph.atlas_r = row[8];
        glyph.atlas_t = row[9];
        // A zero-area plane rectangle is whitespace: advance only, nothing to draw.
        glyph.visible = glyph.plane_r != glyph.plane_l && glyph.plane_t != glyph.plane_b;

        font.glyphs.emplace(static_cast<char32_t>(row[0]), glyph);
    }

    return font;
}

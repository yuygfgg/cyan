#include "sc/source.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace sc {

SourceFile::SourceFile(std::filesystem::path path, std::string text)
    : source_path(std::move(path)), source_text(std::move(text)) {
    line_offsets.push_back(0);
    for (std::size_t index = 0; index < source_text.size(); ++index) {
        if (source_text[index] == '\n') {
            line_offsets.push_back(index + 1);
        }
    }
}

auto SourceFile::load(std::filesystem::path path)
    -> std::expected<SourceFile, std::string> {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected("failed to open input file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return SourceFile(std::move(path), buffer.str());
}

auto SourceFile::fromText(std::filesystem::path path, std::string text)
    -> SourceFile {
    return SourceFile(std::move(path), std::move(text));
}

auto SourceFile::path() const -> const std::filesystem::path& {
    return source_path;
}

auto SourceFile::text() const -> std::string_view { return source_text; }

auto SourceFile::slice(SourceRange range) const -> std::string_view {
    if (range.end < range.begin || range.end > source_text.size()) {
        return {};
    }
    return std::string_view(source_text)
        .substr(range.begin, range.end - range.begin);
}

auto SourceFile::location(std::size_t offset) const -> SourceLocation {
    offset = std::min(offset, source_text.size());

    const auto it = std::ranges::upper_bound(line_offsets, offset);
    const auto line_index =
        static_cast<std::size_t>(std::distance(line_offsets.begin(), it) - 1);
    const auto line_start = line_offsets[line_index];
    return SourceLocation{.line = line_index + 1,
                          .column = offset - line_start + 1};
}

auto SourceFile::range(std::size_t begin, std::size_t end) const
    -> SourceRange {
    return SourceRange{.begin = begin, .end = end, .source = this};
}

} // namespace sc

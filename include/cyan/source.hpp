#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cyan {

class SourceFile;

struct SourceRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    const SourceFile* source = nullptr;
};

struct SourceLocation {
    std::size_t line = 1;
    std::size_t column = 1;
};

class SourceFile {
  public:
    static auto load(std::filesystem::path path)
        -> std::expected<SourceFile, std::string>;
    static auto fromText(std::filesystem::path path, std::string text)
        -> SourceFile;

    [[nodiscard]] auto path() const -> const std::filesystem::path&;
    [[nodiscard]] auto text() const -> std::string_view;
    [[nodiscard]] auto slice(SourceRange range) const -> std::string_view;
    [[nodiscard]] auto location(std::size_t offset) const -> SourceLocation;
    [[nodiscard]] auto range(std::size_t begin, std::size_t end) const
        -> SourceRange;

  private:
    explicit SourceFile(std::filesystem::path path, std::string text);

    std::filesystem::path source_path;
    std::string source_text;
    std::vector<std::size_t> line_offsets;
};

[[nodiscard]] inline auto cover_range(SourceRange begin, SourceRange end)
    -> SourceRange {
    return SourceRange{.begin = begin.begin,
                       .end = end.end,
                       .source =
                           begin.source != nullptr ? begin.source : end.source};
}

} // namespace cyan

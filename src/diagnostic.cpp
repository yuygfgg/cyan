#include "sc/diagnostic.hpp"

#include <algorithm>
#include <ostream>

namespace sc {

Diagnostic::Diagnostic(std::string message, SourceRange range)
    : message_text(std::move(message)), source_range(range) {}

auto Diagnostic::message() const -> const std::string& { return message_text; }

auto Diagnostic::range() const -> SourceRange { return source_range; }

auto Diagnostic::hasRange() const -> bool {
    return source_range.begin != source_range.end;
}

auto Diagnostic::source() const -> const SourceFile* {
    return source_range.source;
}

auto print_diagnostic(std::ostream& stream, const Diagnostic& diagnostic)
    -> void {
    if (diagnostic.source() != nullptr) {
        print_diagnostic(stream, *diagnostic.source(), diagnostic);
        return;
    }

    stream << "error: " << diagnostic.message() << '\n';
}

auto print_diagnostic(std::ostream& stream, const SourceFile& source,
                      const Diagnostic& diagnostic) -> void {
    stream << source.path().string();
    if (diagnostic.hasRange()) {
        const auto location = source.location(diagnostic.range().begin);
        stream << ':' << location.line << ':' << location.column;
    }
    stream << ": error: " << diagnostic.message() << '\n';

    if (!diagnostic.hasRange()) {
        return;
    }

    const auto range = diagnostic.range();
    const auto line_start_offset =
        source.location(range.begin).line == 1
            ? 0U
            : static_cast<unsigned>(
                  source.text().rfind(
                      '\n', std::min(range.begin, source.text().size())) +
                  1U);
    auto line_end_offset = source.text().find('\n', range.begin);
    if (line_end_offset == std::string_view::npos) {
        line_end_offset = source.text().size();
    }

    const auto line = source.text().substr(line_start_offset,
                                           line_end_offset - line_start_offset);
    stream << line << '\n';

    const auto caret_offset = range.begin - line_start_offset;
    for (std::size_t index = 0; index < caret_offset; ++index) {
        stream << (line[index] == '\t' ? '\t' : ' ');
    }

    const auto caret_width = std::max<std::size_t>(1, range.end - range.begin);
    for (std::size_t index = 0; index < caret_width; ++index) {
        stream << '^';
    }
    stream << '\n';
}

auto print_diagnostics(std::ostream& stream, const DiagnosticList& diagnostics)
    -> void {
    for (const auto& diagnostic : diagnostics) {
        print_diagnostic(stream, diagnostic);
    }
}

} // namespace sc

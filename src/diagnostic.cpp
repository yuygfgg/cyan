#include "cyan/diagnostic.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <string_view>

namespace cyan {

namespace {

auto has_range(SourceRange range) -> bool { return range.begin != range.end; }

auto context_prefix(DiagnosticContext::Kind kind) -> std::string_view {
    switch (kind) {
    case DiagnosticContext::Kind::Note:
        return "note";
    case DiagnosticContext::Kind::Help:
        return "help";
    }
    return "note";
}

auto print_source_excerpt(std::ostream& stream, const SourceFile& source,
                          SourceRange range) -> void {
    const auto location = source.location(range.begin);
    stream << " --> " << source.path().string() << ':' << location.line << ':'
           << location.column << '\n';

    const auto line_number = std::to_string(location.line);
    const auto gutter_width = line_number.size();
    stream << std::string(gutter_width, ' ') << " |\n";

    const auto line_start_offset =
        location.line == 1
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
    stream << line_number << " | " << line << '\n';
    stream << std::string(gutter_width, ' ') << " | ";

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

auto print_context(std::ostream& stream, const DiagnosticContext& context)
    -> void {
    stream << context_prefix(context.kind) << ": " << context.message << '\n';
    if (context.range.source != nullptr && has_range(context.range)) {
        print_source_excerpt(stream, *context.range.source, context.range);
    }
}

auto print_diagnostic_body(std::ostream& stream, const Diagnostic& diagnostic,
                           const SourceFile* fallback_source) -> void {
    stream << "error: " << diagnostic.message() << '\n';

    if (diagnostic.hasRange()) {
        const auto* source = diagnostic.source() != nullptr
                                 ? diagnostic.source()
                                 : fallback_source;
        if (source != nullptr) {
            print_source_excerpt(stream, *source, diagnostic.range());
        }
    }

    for (const auto& context : diagnostic.contexts()) {
        print_context(stream, context);
    }
}

} // namespace

Diagnostic::Diagnostic(std::string message, SourceRange range)
    : message_text(std::move(message)), source_range(range) {}

auto Diagnostic::message() const -> const std::string& { return message_text; }

auto Diagnostic::range() const -> SourceRange { return source_range; }

auto Diagnostic::hasRange() const -> bool { return has_range(source_range); }

auto Diagnostic::source() const -> const SourceFile* {
    return source_range.source;
}

auto Diagnostic::contexts() const -> const std::vector<DiagnosticContext>& {
    return diagnostic_contexts;
}

auto Diagnostic::addNote(std::string message, SourceRange range)
    -> Diagnostic& {
    diagnostic_contexts.push_back(DiagnosticContext{
        .kind = DiagnosticContext::Kind::Note,
        .message = std::move(message),
        .range = range,
    });
    return *this;
}

auto Diagnostic::addHelp(std::string message, SourceRange range)
    -> Diagnostic& {
    diagnostic_contexts.push_back(DiagnosticContext{
        .kind = DiagnosticContext::Kind::Help,
        .message = std::move(message),
        .range = range,
    });
    return *this;
}

auto print_diagnostic(std::ostream& stream, const Diagnostic& diagnostic)
    -> void {
    print_diagnostic_body(stream, diagnostic, nullptr);
}

auto print_diagnostic(std::ostream& stream, const SourceFile& source,
                      const Diagnostic& diagnostic) -> void {
    print_diagnostic_body(stream, diagnostic, &source);
}

auto print_diagnostics(std::ostream& stream, const DiagnosticList& diagnostics)
    -> void {
    for (const auto& diagnostic : diagnostics) {
        print_diagnostic(stream, diagnostic);
    }
}

} // namespace cyan

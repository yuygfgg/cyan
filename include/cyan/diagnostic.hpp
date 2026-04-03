#pragma once

#include "cyan/source.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace cyan {

struct DiagnosticContext {
    enum class Kind : std::uint8_t {
        Note,
        Help,
    };

    Kind kind = Kind::Note;
    std::string message;
    SourceRange range;
};

class Diagnostic {
  public:
    Diagnostic(std::string message, SourceRange range = {});

    [[nodiscard]] auto message() const -> const std::string&;
    [[nodiscard]] auto range() const -> SourceRange;
    [[nodiscard]] auto hasRange() const -> bool;
    [[nodiscard]] auto source() const -> const SourceFile*;
    [[nodiscard]] auto contexts() const
        -> const std::vector<DiagnosticContext>&;

    auto addNote(std::string message, SourceRange range = {}) -> Diagnostic&;
    auto addHelp(std::string message, SourceRange range = {}) -> Diagnostic&;

  private:
    std::string message_text;
    SourceRange source_range;
    std::vector<DiagnosticContext> diagnostic_contexts;
};

using DiagnosticList = std::vector<Diagnostic>;

auto print_diagnostic(std::ostream& stream, const Diagnostic& diagnostic)
    -> void;
auto print_diagnostic(std::ostream& stream, const SourceFile& source,
                      const Diagnostic& diagnostic) -> void;
auto print_diagnostics(std::ostream& stream, const DiagnosticList& diagnostics)
    -> void;

} // namespace cyan

#pragma once

#include "sc/source.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace sc {

class Diagnostic {
  public:
    Diagnostic(std::string message, SourceRange range = {});

    [[nodiscard]] auto message() const -> const std::string&;
    [[nodiscard]] auto range() const -> SourceRange;
    [[nodiscard]] auto hasRange() const -> bool;
    [[nodiscard]] auto source() const -> const SourceFile*;

  private:
    std::string message_text;
    SourceRange source_range;
};

using DiagnosticList = std::vector<Diagnostic>;

auto print_diagnostic(std::ostream& stream, const Diagnostic& diagnostic)
    -> void;
auto print_diagnostic(std::ostream& stream, const SourceFile& source,
                      const Diagnostic& diagnostic) -> void;
auto print_diagnostics(std::ostream& stream, const DiagnosticList& diagnostics)
    -> void;

} // namespace sc

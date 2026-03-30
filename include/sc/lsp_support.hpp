#pragma once

#include "sc/ast.hpp"
#include "sc/sema.hpp"
#include "sc/source.hpp"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace sc {

enum class LSPSymbolKind : std::uint8_t {
    BuiltinType,
    Struct,
    Enum,
    Interface,
    Function,
    Field,
    Variant,
    TypeParameter,
    Parameter,
    Local,
    SwitchBinding,
    ImportModule,
};

enum class LSPSymbolRole : std::uint8_t {
    Declaration,
    Reference,
};

struct LSPSymbolOccurrence {
    LSPSymbolKind kind = LSPSymbolKind::Local;
    LSPSymbolRole role = LSPSymbolRole::Reference;
    std::string name;
    SourceRange range;
    std::optional<SourceRange> declaration_range;
    const Type* type = nullptr;
};

struct LSPQueryResult {
    const ast::Module* module = nullptr;
    const ast::Decl* top_level_decl = nullptr;
    const ast::FunctionDecl* enclosing_function = nullptr;
    const ast::Stmt* statement = nullptr;
    const ast::Expr* expression = nullptr;
    const ast::TypeSyntax* type_syntax = nullptr;
    std::optional<LSPSymbolOccurrence> symbol;
};

class LSPSupport {
  public:
    LSPSupport(const ast::Package& package, const SemanticAnalysis& analysis);

    [[nodiscard]] auto moduleFor(const SourceFile& source) const
        -> const ast::Module*;
    [[nodiscard]] auto offsetForLocation(const SourceFile& source,
                                         SourceLocation location) const
        -> std::optional<std::size_t>;
    [[nodiscard]] auto query(const SourceFile& source, std::size_t offset) const
        -> std::optional<LSPQueryResult>;
    [[nodiscard]] auto query(const SourceFile& source,
                             SourceLocation location) const
        -> std::optional<LSPQueryResult>;
    [[nodiscard]] auto documentSymbols(const SourceFile& source) const
        -> std::vector<LSPSymbolOccurrence>;

  private:
    const ast::Package& package;
    const SemanticAnalysis& analysis;
};

class LanguageServer {
  public:
    auto run(std::istream& input, std::ostream& output) -> int;
};

} // namespace sc

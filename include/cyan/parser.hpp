#pragma once

#include "cyan/ast.hpp"
#include "cyan/diagnostic.hpp"
#include "cyan/lexer.hpp"

#include <expected>
#include <vector>

namespace cyan {

class Parser {
  public:
    Parser(const SourceFile& source_file, std::vector<Token> tokens);

    auto parseModule() -> ast::Module;

  private:
    enum class SyncContext : std::uint8_t {
        TopLevel,
        Block,
        SwitchCaseHeader,
        SwitchCaseBody,
        StructBody,
        EnumBody,
        ExternBlock,
        ParameterList,
    };

    auto parseDecls(bool is_export)
        -> std::expected<std::vector<ast::Decl>, Diagnostic>;
    auto parseImportDecl(bool is_export)
        -> std::expected<ast::ImportDecl, Diagnostic>;
    auto parseExternDecls()
        -> std::expected<std::vector<ast::Decl>, Diagnostic>;
    auto parseInterfaceDecl(bool is_export)
        -> std::expected<ast::InterfaceDecl, Diagnostic>;
    auto parseStructDecls(bool is_export)
        -> std::expected<std::vector<ast::Decl>, Diagnostic>;
    auto parseEnumDecls(bool is_export)
        -> std::expected<std::vector<ast::Decl>, Diagnostic>;
    auto parseImplDecl(bool is_export)
        -> std::expected<ast::FunctionDecl, Diagnostic>;
    auto parseFunctionDecl(bool is_export, bool is_extern)
        -> std::expected<ast::FunctionDecl, Diagnostic>;
    auto parseParameterList(std::vector<ast::Parameter>& parameters)
        -> std::expected<void, Diagnostic>;
    auto parseBlock() -> std::expected<ast::BlockPtr, Diagnostic>;
    auto parseStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseVarDeclStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseIfStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseWhileStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseForStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseBreakStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseContinueStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseUncheckedStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseSwitchStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseSwitchCase() -> std::expected<ast::SwitchCase, Diagnostic>;
    auto parseReturnStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseDropStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseExprOrAssignStmt() -> std::expected<ast::StmtPtr, Diagnostic>;
    auto parseSimpleStmt(TokenKind terminator, std::string message)
        -> std::expected<ast::StmtPtr, Diagnostic>;
    [[nodiscard]] auto looksLikeVarDecl() -> bool;
    [[nodiscard]] auto looksLikeExplicitCallTypeArguments() -> bool;
    [[nodiscard]] auto looksLikeSharedTypeQualifier() -> bool;
    [[nodiscard]] auto typeSyntaxContainsPointer(const ast::TypeSyntax& type)
        const -> bool;
    auto parseType() -> std::expected<ast::TypeSyntaxPtr, Diagnostic>;
    auto parseTypeArguments()
        -> std::expected<std::vector<ast::TypeSyntaxPtr>, Diagnostic>;
    auto parseTypeParameters() -> std::expected<std::vector<Token>, Diagnostic>;
    auto parseExpr() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseLogicalOr() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseLogicalAnd() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseBitwiseOr() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseBitwiseXor() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseBitwiseAnd() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseEquality() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseRelational() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseShift() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseAdditive() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseMultiplicative() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseCast() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseUnary() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parsePostfix() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parsePrimary() -> std::expected<ast::ExprPtr, Diagnostic>;
    auto parseIdentifier() -> std::expected<Token, Diagnostic>;
    auto parseInterfaceName() -> std::expected<Token, Diagnostic>;
    auto parseDependencyPath()
        -> std::expected<ast::DependencyPath, Diagnostic>;
    auto parseDependsClause(ast::FunctionDecl& decl)
        -> std::expected<void, Diagnostic>;

    [[nodiscard]] auto current() const -> const Token&;
    [[nodiscard]] auto previous() const -> const Token&;
    [[nodiscard]] auto isAtEnd() const -> bool;
    [[nodiscard]] auto check(TokenKind kind) const -> bool;
    auto advance() -> const Token&;
    auto match(TokenKind kind) -> bool;
    auto expect(TokenKind kind, std::string message)
        -> std::expected<Token, Diagnostic>;
    auto report(Diagnostic diagnostic) -> void;
    auto synchronize(SyncContext context, bool progressed) -> void;
    [[nodiscard]] auto canStartType(TokenKind kind) const -> bool;
    [[nodiscard]] auto canStartExpr(TokenKind kind) const -> bool;
    [[nodiscard]] auto canStartStmt(TokenKind kind) const -> bool;
    [[nodiscard]] auto canStartTopLevelDecl(TokenKind kind) const -> bool;
    [[nodiscard]] auto isSyncBoundary(SyncContext context, TokenKind kind) const
        -> bool;
    auto binaryExprTail(
        ast::ExprPtr expr,
        const std::vector<std::pair<TokenKind, ast::BinaryOp>>& ops,
        std::expected<ast::ExprPtr, Diagnostic> (Parser::*subparser)())
        -> std::expected<ast::ExprPtr, Diagnostic>;
    std::vector<Token> tokens;
    const SourceFile& source_file;
    std::size_t index = 0;
    DiagnosticList diagnostics;
};

} // namespace cyan

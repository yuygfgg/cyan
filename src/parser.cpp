#include "cyan/parser.hpp"

#include <cstdlib>
#include <optional>
#include <string>

namespace cyan {

namespace {

template <typename T>
auto make_expr(SourceRange range, T node) -> ast::ExprPtr {
    auto expr = std::make_unique<ast::Expr>();
    expr->range = range;
    expr->node = std::move(node);
    return expr;
}

template <typename T>
auto make_stmt(SourceRange range, T node) -> ast::StmtPtr {
    auto stmt = std::make_unique<ast::Stmt>();
    stmt->range = range;
    stmt->node = std::move(node);
    return stmt;
}

auto decode_string_literal(std::string_view text)
    -> std::expected<std::string, std::string> {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return std::unexpected("invalid string literal");
    }

    std::string value;
    value.reserve(text.size() - 2);
    for (std::size_t index = 1; index + 1 < text.size(); ++index) {
        const auto ch = text[index];
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (index + 2 >= text.size()) {
            return std::unexpected("unterminated string escape");
        }
        ++index;
        switch (text[index]) {
        case '"':
            value.push_back('"');
            break;
        case '\\':
            value.push_back('\\');
            break;
        case 'n':
            value.push_back('\n');
            break;
        case 'r':
            value.push_back('\r');
            break;
        case 't':
            value.push_back('\t');
            break;
        case '0':
            value.push_back('\0');
            break;
        default:
            return std::unexpected("unsupported string escape");
        }
    }
    return value;
}

} // namespace

Parser::Parser(const SourceFile& source_file, std::vector<Token> tokens)
    : tokens(std::move(tokens)), source_file(source_file) {}

auto Parser::parseModule() -> ast::Module {
    ast::Module module;
    module.source = &source_file;
    while (!isAtEnd()) {
        if (check(TokenKind::KwImport)) {
            const auto start_index = index;
            auto import_decl = parseImportDecl();
            if (!import_decl) {
                report(import_decl.error());
                synchronize(SyncContext::TopLevel, index != start_index);
                continue;
            }
            module.imports.push_back(std::move(*import_decl));
            continue;
        }

        const auto is_export = match(TokenKind::KwExport);
        if (check(TokenKind::KwExtern)) {
            const auto start_index = index;
            auto decls = parseExternDecls();
            if (!decls) {
                report(decls.error());
                synchronize(SyncContext::TopLevel, index != start_index);
                continue;
            }
            for (auto& decl : *decls) {
                if (auto* function = std::get_if<ast::FunctionDecl>(&decl);
                    function != nullptr) {
                    function->is_export = is_export;
                }
                module.declarations.push_back(std::move(decl));
            }
            continue;
        }

        const auto start_index = index;
        auto decls = parseDecls(is_export);
        if (!decls) {
            report(decls.error());
            synchronize(SyncContext::TopLevel, index != start_index);
            continue;
        }
        for (auto& decl : *decls) {
            module.declarations.push_back(std::move(decl));
        }
    }
    module.diagnostics = std::move(diagnostics);
    return module;
}

auto Parser::parseDecls(bool is_export)
    -> std::expected<std::vector<ast::Decl>, Diagnostic> {
    if (check(TokenKind::KwImpl)) {
        auto decl = parseImplDecl(is_export);
        if (!decl) {
            return std::unexpected(decl.error());
        }
        std::vector<ast::Decl> decls;
        decls.emplace_back(std::move(*decl));
        return decls;
    }
    if (check(TokenKind::KwInterface)) {
        auto decl = parseInterfaceDecl(is_export);
        if (!decl) {
            return std::unexpected(decl.error());
        }
        std::vector<ast::Decl> decls;
        decls.emplace_back(std::move(*decl));
        return decls;
    }
    if (check(TokenKind::KwStruct)) {
        auto decls = parseStructDecls(is_export);
        if (!decls) {
            return std::unexpected(decls.error());
        }
        return decls;
    }
    if (check(TokenKind::KwEnum)) {
        auto decls = parseEnumDecls(is_export);
        if (!decls) {
            return std::unexpected(decls.error());
        }
        return decls;
    }

    auto decl = parseFunctionDecl(is_export, false);
    if (!decl) {
        return std::unexpected(decl.error());
    }
    std::vector<ast::Decl> decls;
    decls.emplace_back(std::move(*decl));
    return decls;
}

auto Parser::parseInterfaceDecl(bool is_export)
    -> std::expected<ast::InterfaceDecl, Diagnostic> {
    const auto begin = advance().range.begin;
    auto type_parameters = parseTypeParameters();
    if (!type_parameters) {
        return std::unexpected(type_parameters.error());
    }
    if (type_parameters->size() != 1) {
        return std::unexpected(
            Diagnostic("interface declarations require exactly one receiver "
                       "type parameter",
                       source_file.range(begin, previous().range.end)));
    }

    auto return_type = parseType();
    if (!return_type) {
        return std::unexpected(return_type.error());
    }
    auto name = parseIdentifier();
    if (!name) {
        return std::unexpected(name.error());
    }

    ast::InterfaceDecl decl;
    decl.range = source_file.range(begin, return_type->get()->range.begin);
    decl.receiver_type_parameter = type_parameters->front().text;
    decl.receiver_type_parameter_range = type_parameters->front().range;
    decl.return_type = std::move(*return_type);
    decl.name = name->text;
    decl.name_range = name->range;
    decl.is_export = is_export;

    auto lparen =
        expect(TokenKind::LParen, "expected '(' after interface name");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }
    if (!check(TokenKind::RParen)) {
        auto parameters = parseParameterList(decl.parameters);
        if (!parameters) {
            return std::unexpected(parameters.error());
        }
    }
    auto rparen =
        expect(TokenKind::RParen, "expected ')' after interface parameters");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }
    auto semicolon = expect(TokenKind::Semicolon,
                            "expected ';' after interface declaration");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }
    decl.range.end = semicolon->range.end;
    return decl;
}

auto Parser::parseImportDecl() -> std::expected<ast::ImportDecl, Diagnostic> {
    const auto begin = advance().range.begin;
    auto first = parseIdentifier();
    if (!first) {
        return std::unexpected(first.error());
    }

    std::string module_name = first->text;
    std::vector<SourceRange> module_name_part_ranges{first->range};
    auto module_name_range = first->range;
    while (match(TokenKind::Dot)) {
        auto part = parseIdentifier();
        if (!part) {
            return std::unexpected(part.error());
        }
        module_name += '.';
        module_name += part->text;
        module_name_part_ranges.push_back(part->range);
        module_name_range.end = part->range.end;
    }

    auto semicolon = expect(TokenKind::Semicolon, "expected ';' after import");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }

    ast::ImportDecl import_decl;
    import_decl.range = source_file.range(begin, semicolon->range.end);
    import_decl.module_name = std::move(module_name);
    import_decl.module_name_range = module_name_range;
    import_decl.module_name_part_ranges = std::move(module_name_part_ranges);
    return import_decl;
}

auto Parser::parseExternDecls()
    -> std::expected<std::vector<ast::Decl>, Diagnostic> {
    advance();
    auto lbrace = expect(TokenKind::LBrace, "expected '{' after extern");
    if (!lbrace) {
        return std::unexpected(lbrace.error());
    }

    std::vector<ast::Decl> decls;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        const auto start_index = index;
        auto decl = parseFunctionDecl(false, true);
        if (!decl) {
            report(decl.error());
            synchronize(SyncContext::ExternBlock, index != start_index);
            continue;
        }
        decls.emplace_back(std::move(*decl));
    }

    if (!check(TokenKind::RBrace)) {
        report(Diagnostic("expected '}' after extern block", current().range));
        return decls;
    }
    advance();
    return decls;
}

auto Parser::parseStructDecls(bool is_export)
    -> std::expected<std::vector<ast::Decl>, Diagnostic> {
    const auto begin = advance().range.begin;
    auto name = parseIdentifier();
    if (!name) {
        return std::unexpected(name.error());
    }
    ast::StructDecl decl;
    decl.range = source_file.range(begin, begin);
    decl.name = name->text;
    decl.name_range = name->range;
    decl.is_export = is_export;
    if (check(TokenKind::Less)) {
        auto type_parameters = parseTypeParameters();
        if (!type_parameters) {
            return std::unexpected(type_parameters.error());
        }
        decl.type_parameters.reserve(type_parameters->size());
        decl.type_parameter_ranges.reserve(type_parameters->size());
        for (const auto& type_parameter : *type_parameters) {
            decl.type_parameters.push_back(type_parameter.text);
            decl.type_parameter_ranges.push_back(type_parameter.range);
        }
    }
    auto lbrace = expect(TokenKind::LBrace, "expected '{' after struct name");
    if (!lbrace) {
        return std::unexpected(lbrace.error());
    }

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (check(TokenKind::KwImpl)) {
            return std::unexpected(Diagnostic(
                "impl declarations must appear at top level", current().range));
        }

        const auto start_index = index;
        auto field_type = parseType();
        if (!field_type) {
            report(field_type.error());
            synchronize(SyncContext::StructBody, index != start_index);
            continue;
        }
        auto field_name = parseIdentifier();
        if (!field_name) {
            report(field_name.error());
            synchronize(SyncContext::StructBody, index != start_index);
            continue;
        }

        SourceRange field_end_range = field_name->range;
        if (!match(TokenKind::Semicolon)) {
            report(
                Diagnostic("expected ';' after struct field", current().range));
        } else {
            field_end_range = previous().range;
        }

        ast::StructField field;
        field.range = source_file.range(field_type->get()->range.begin,
                                        field_end_range.end);
        field.type = std::move(*field_type);
        field.name = field_name->text;
        field.name_range = field_name->range;
        decl.fields.push_back(std::move(field));
    }

    if (!check(TokenKind::RBrace)) {
        report(Diagnostic("expected '}' after struct body", current().range));
    } else {
        advance();
    }

    auto end = current().range.begin;
    if (index > 0) {
        end = previous().range.end;
    }
    if (!match(TokenKind::Semicolon)) {
        report(Diagnostic("expected ';' after struct declaration",
                          current().range));
    } else {
        end = previous().range.end;
    }

    decl.range.end = end;
    std::vector<ast::Decl> decls;
    decls.emplace_back(std::move(decl));
    return decls;
}

auto Parser::parseEnumDecls(bool is_export)
    -> std::expected<std::vector<ast::Decl>, Diagnostic> {
    const auto begin = advance().range.begin;
    auto name = parseIdentifier();
    if (!name) {
        return std::unexpected(name.error());
    }
    ast::EnumDecl decl;
    decl.range = source_file.range(begin, begin);
    decl.name = name->text;
    decl.name_range = name->range;
    decl.is_export = is_export;
    if (check(TokenKind::Less)) {
        auto type_parameters = parseTypeParameters();
        if (!type_parameters) {
            return std::unexpected(type_parameters.error());
        }
        decl.type_parameters.reserve(type_parameters->size());
        decl.type_parameter_ranges.reserve(type_parameters->size());
        for (const auto& type_parameter : *type_parameters) {
            decl.type_parameters.push_back(type_parameter.text);
            decl.type_parameter_ranges.push_back(type_parameter.range);
        }
    }
    auto lbrace = expect(TokenKind::LBrace, "expected '{' after enum name");
    if (!lbrace) {
        return std::unexpected(lbrace.error());
    }

    std::uint32_t next_tag = 0;
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        if (check(TokenKind::KwImpl)) {
            return std::unexpected(Diagnostic(
                "impl declarations must appear at top level", current().range));
        }

        const auto start_index = index;
        auto variant_name = parseIdentifier();
        if (!variant_name) {
            report(variant_name.error());
            synchronize(SyncContext::EnumBody, index != start_index);
            continue;
        }

        ast::EnumVariant variant;
        variant.range = variant_name->range;
        variant.name = variant_name->text;
        variant.name_range = variant_name->range;
        variant.tag = next_tag++;

        if (match(TokenKind::LParen)) {
            auto payload_type = parseType();
            if (!payload_type) {
                report(payload_type.error());
                synchronize(SyncContext::EnumBody, true);
                continue;
            }
            auto rparen = expect(TokenKind::RParen,
                                 "expected ')' after variant payload type");
            if (!rparen) {
                report(rparen.error());
                synchronize(SyncContext::EnumBody, true);
                continue;
            }
            variant.payload_type = std::move(*payload_type);
            variant.range.end = rparen->range.end;
        } else {
            variant.range.end = variant_name->range.end;
        }

        decl.variants.push_back(std::move(variant));
        if (!check(TokenKind::RBrace) && !match(TokenKind::Comma)) {
            report(Diagnostic("expected ',' or '}' after enum member",
                              current().range));
        }
    }

    if (!check(TokenKind::RBrace)) {
        report(Diagnostic("expected '}' after enum body", current().range));
    } else {
        advance();
    }

    auto end = current().range.begin;
    if (index > 0) {
        end = previous().range.end;
    }
    if (!match(TokenKind::Semicolon)) {
        report(
            Diagnostic("expected ';' after enum declaration", current().range));
    } else {
        end = previous().range.end;
    }
    decl.range.end = end;
    std::vector<ast::Decl> decls;
    decls.emplace_back(std::move(decl));
    return decls;
}

auto Parser::parseFunctionDecl(bool is_export, bool is_extern)
    -> std::expected<ast::FunctionDecl, Diagnostic> {
    auto return_type = parseType();
    if (!return_type) {
        return std::unexpected(return_type.error());
    }
    auto name = parseIdentifier();
    if (!name) {
        return std::unexpected(name.error());
    }
    ast::FunctionDecl decl;
    decl.range = source_file.range(return_type->get()->range.begin,
                                   return_type->get()->range.begin);
    decl.return_type = std::move(*return_type);
    decl.name = name->text;
    decl.name_range = name->range;
    decl.is_export = is_export;
    decl.is_extern = is_extern;
    if (decl.name.rfind("drop_", 0) == 0) {
        return std::unexpected(Diagnostic(
            "drop implementations must be declared as 'impl drop(...)' "
            "at top level",
            name->range));
    }
    if (check(TokenKind::Less)) {
        auto type_parameters = parseTypeParameters();
        if (!type_parameters) {
            return std::unexpected(type_parameters.error());
        }
        decl.type_parameters.reserve(type_parameters->size());
        decl.type_parameter_ranges.reserve(type_parameters->size());
        for (const auto& type_parameter : *type_parameters) {
            decl.type_parameters.push_back(type_parameter.text);
            decl.type_parameter_ranges.push_back(type_parameter.range);
        }
    }
    auto lparen = expect(TokenKind::LParen, "expected '(' after function name");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }

    if (!check(TokenKind::RParen)) {
        auto parameters = parseParameterList(decl.parameters);
        if (!parameters) {
            return std::unexpected(parameters.error());
        }
    }

    auto rparen = expect(TokenKind::RParen, "expected ')' after parameters");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }

    auto depends = parseDependsClause(decl);
    if (!depends) {
        return std::unexpected(depends.error());
    }

    if (is_extern) {
        if (!match(TokenKind::Semicolon)) {
            report(Diagnostic("expected ';' after extern prototype",
                              current().range));
            decl.range.end = name->range.end;
            return decl;
        }
        decl.range.end = previous().range.end;
        return decl;
    }

    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }
    decl.range.end = (*body)->range.end;
    decl.body = std::move(*body);
    return decl;
}

auto Parser::parseParameterList(std::vector<ast::Parameter>& parameters)
    -> std::expected<void, Diagnostic> {
    while (!check(TokenKind::RParen) && !isAtEnd()) {
        const auto start_index = index;
        ast::Parameter parameter;
        auto param_type = parseType();
        if (!param_type) {
            report(param_type.error());
            synchronize(SyncContext::ParameterList, index != start_index);
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        auto param_name = parseIdentifier();
        if (!param_name) {
            report(param_name.error());
            synchronize(SyncContext::ParameterList, index != start_index);
            if (match(TokenKind::Comma)) {
                continue;
            }
            break;
        }
        parameter.range = source_file.range(param_type->get()->range.begin,
                                            param_name->range.end);
        parameter.type = std::move(*param_type);
        parameter.name = param_name->text;
        parameter.name_range = param_name->range;
        parameters.push_back(std::move(parameter));

        if (check(TokenKind::RParen)) {
            break;
        }
        if (match(TokenKind::Comma)) {
            continue;
        }
        report(
            Diagnostic("expected ',' or ')' after parameter", current().range));
    }
    return {};
}

auto Parser::parseImplDecl(bool is_export)
    -> std::expected<ast::FunctionDecl, Diagnostic> {
    const auto impl_token = advance();
    auto interface_name = parseInterfaceName();
    if (!interface_name) {
        return std::unexpected(interface_name.error());
    }

    ast::FunctionDecl decl;
    decl.range =
        source_file.range(impl_token.range.begin, interface_name->range.end);
    decl.name = interface_name->text;
    decl.name_range = interface_name->range;
    decl.impl_target_kind = ast::ImplTargetKind::Named;
    decl.is_export = is_export;

    if (check(TokenKind::Less)) {
        auto type_parameters = parseTypeParameters();
        if (!type_parameters) {
            return std::unexpected(type_parameters.error());
        }
        decl.type_parameters.reserve(type_parameters->size());
        decl.type_parameter_ranges.reserve(type_parameters->size());
        for (const auto& type_parameter : *type_parameters) {
            decl.type_parameters.push_back(type_parameter.text);
            decl.type_parameter_ranges.push_back(type_parameter.range);
        }
    }

    auto lparen = expect(TokenKind::LParen, "expected '(' after impl name");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }

    if (!check(TokenKind::RParen)) {
        auto parameters = parseParameterList(decl.parameters);
        if (!parameters) {
            return std::unexpected(parameters.error());
        }
    }

    auto rparen = expect(TokenKind::RParen, "expected ')' after parameters");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }

    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }
    if (!decl.parameters.empty()) {
        const auto* receiver_type = decl.parameters.front().type.get();
        if (receiver_type != nullptr &&
            receiver_type->kind == ast::TypeSyntax::Kind::Borrow &&
            receiver_type->element_type != nullptr &&
            receiver_type->element_type->kind == ast::TypeSyntax::Kind::Named) {
            decl.impl_target_name = receiver_type->element_type->name;
        }
    }
    decl.range.end = (*body)->range.end;
    decl.body = std::move(*body);
    return decl;
}

auto Parser::parseBlock() -> std::expected<ast::BlockPtr, Diagnostic> {
    auto lbrace = expect(TokenKind::LBrace, "expected '{'");
    if (!lbrace) {
        return std::unexpected(lbrace.error());
    }

    auto block = std::make_unique<ast::Block>();
    block->range = source_file.range(lbrace->range.begin, lbrace->range.begin);
    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        const auto start_index = index;
        auto stmt = parseStmt();
        if (!stmt) {
            report(stmt.error());
            synchronize(SyncContext::Block, index != start_index);
            continue;
        }
        block->statements.push_back(std::move(*stmt));
    }

    if (!check(TokenKind::RBrace)) {
        report(Diagnostic("expected '}'", current().range));
        block->range.end = block->statements.empty()
                               ? lbrace->range.end
                               : block->statements.back()->range.end;
        return block;
    }
    block->range.end = advance().range.end;
    return block;
}

auto Parser::parseStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    if (check(TokenKind::LBrace)) {
        auto block = parseBlock();
        if (!block) {
            return std::unexpected(block.error());
        }
        auto stmt = std::make_unique<ast::Stmt>();
        stmt->range = (*block)->range;
        stmt->node = std::move(*(*block));
        return stmt;
    }
    if (match(TokenKind::KwIf)) {
        --index;
        return parseIfStmt();
    }
    if (match(TokenKind::KwWhile)) {
        --index;
        return parseWhileStmt();
    }
    if (match(TokenKind::KwFor)) {
        --index;
        return parseForStmt();
    }
    if (match(TokenKind::KwBreak)) {
        --index;
        return parseBreakStmt();
    }
    if (match(TokenKind::KwContinue)) {
        --index;
        return parseContinueStmt();
    }
    if (match(TokenKind::KwUnchecked)) {
        --index;
        return parseUncheckedStmt();
    }
    if (match(TokenKind::KwSwitch)) {
        --index;
        return parseSwitchStmt();
    }
    if (match(TokenKind::KwReturn)) {
        --index;
        return parseReturnStmt();
    }
    if (match(TokenKind::KwDrop)) {
        --index;
        return parseDropStmt();
    }
    if (looksLikeVarDecl()) {
        return parseVarDeclStmt();
    }

    return parseExprOrAssignStmt();
}

auto Parser::parseVarDeclStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    ast::VarDeclStmt stmt;
    const auto begin = current().range.begin;

    auto type = parseType();
    if (!type) {
        return std::unexpected(type.error());
    }
    auto name = parseIdentifier();
    if (!name) {
        return std::unexpected(name.error());
    }

    stmt.type = std::move(*type);
    stmt.name = name->text;
    stmt.name_range = name->range;
    if (match(TokenKind::Equal)) {
        auto initializer = parseExpr();
        if (!initializer) {
            return std::unexpected(initializer.error());
        }
        stmt.initializer = std::move(*initializer);
    }

    auto semicolon =
        expect(TokenKind::Semicolon, "expected ';' after declaration");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }
    return make_stmt(source_file.range(begin, semicolon->range.end),
                     std::move(stmt));
}

auto Parser::parseIfStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto lparen = expect(TokenKind::LParen, "expected '(' after if");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }
    auto condition = parseExpr();
    if (!condition) {
        return std::unexpected(condition.error());
    }
    auto rparen = expect(TokenKind::RParen, "expected ')' after condition");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }
    auto then_block = parseBlock();
    if (!then_block) {
        return std::unexpected(then_block.error());
    }

    ast::IfStmt stmt;
    stmt.condition = std::move(*condition);
    stmt.then_block = std::move(*then_block);
    if (match(TokenKind::KwElse)) {
        auto else_block = parseBlock();
        if (!else_block) {
            return std::unexpected(else_block.error());
        }
        stmt.else_block = std::move(*else_block);
    }

    const auto end = stmt.else_block != nullptr ? stmt.else_block->range.end
                                                : stmt.then_block->range.end;
    return make_stmt(source_file.range(begin, end), std::move(stmt));
}

auto Parser::parseWhileStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto lparen = expect(TokenKind::LParen, "expected '(' after while");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }
    auto condition = parseExpr();
    if (!condition) {
        return std::unexpected(condition.error());
    }
    auto rparen = expect(TokenKind::RParen, "expected ')' after condition");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }
    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }

    ast::WhileStmt stmt;
    stmt.condition = std::move(*condition);
    stmt.body = std::move(*body);
    return make_stmt(source_file.range(begin, stmt.body->range.end),
                     std::move(stmt));
}

auto Parser::parseForStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto lparen = expect(TokenKind::LParen, "expected '(' after for");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }

    ast::ForStmt stmt;
    if (check(TokenKind::Semicolon)) {
        advance();
    } else if (looksLikeVarDecl()) {
        auto initializer = parseVarDeclStmt();
        if (!initializer) {
            return std::unexpected(initializer.error());
        }
        stmt.initializer = std::move(*initializer);
    } else {
        auto initializer = parseSimpleStmt(
            TokenKind::Semicolon, "expected ';' after for initializer");
        if (!initializer) {
            return std::unexpected(initializer.error());
        }
        stmt.initializer = std::move(*initializer);
    }

    if (!check(TokenKind::Semicolon)) {
        auto condition = parseExpr();
        if (!condition) {
            return std::unexpected(condition.error());
        }
        stmt.condition = std::move(*condition);
    }
    auto condition_semicolon =
        expect(TokenKind::Semicolon, "expected ';' after for condition");
    if (!condition_semicolon) {
        return std::unexpected(condition_semicolon.error());
    }

    if (!check(TokenKind::RParen)) {
        auto step = parseSimpleStmt(TokenKind::RParen,
                                    "expected ')' after for clauses");
        if (!step) {
            return std::unexpected(step.error());
        }
        stmt.step = std::move(*step);
    } else {
        advance();
    }

    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }
    stmt.body = std::move(*body);
    return make_stmt(source_file.range(begin, stmt.body->range.end),
                     std::move(stmt));
}

auto Parser::parseBreakStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto semicolon = expect(TokenKind::Semicolon, "expected ';' after break");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }
    return make_stmt(source_file.range(begin, semicolon->range.end),
                     ast::BreakStmt{});
}

auto Parser::parseContinueStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto semicolon =
        expect(TokenKind::Semicolon, "expected ';' after continue");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }
    return make_stmt(source_file.range(begin, semicolon->range.end),
                     ast::ContinueStmt{});
}

auto Parser::parseUncheckedStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto body = parseBlock();
    if (!body) {
        return std::unexpected(body.error());
    }
    ast::UncheckedStmt stmt;
    stmt.body = std::move(*body);
    return make_stmt(source_file.range(begin, stmt.body->range.end),
                     std::move(stmt));
}

auto Parser::parseSwitchStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto lparen = expect(TokenKind::LParen, "expected '(' after switch");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }
    auto scrutinee = parseExpr();
    if (!scrutinee) {
        return std::unexpected(scrutinee.error());
    }
    auto rparen =
        expect(TokenKind::RParen, "expected ')' after switch scrutinee");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }
    auto lbrace = expect(TokenKind::LBrace, "expected '{' before switch cases");
    if (!lbrace) {
        return std::unexpected(lbrace.error());
    }

    ast::SwitchStmt stmt;
    stmt.scrutinee = std::move(*scrutinee);

    while (!check(TokenKind::RBrace) && !isAtEnd()) {
        const auto start_index = index;
        auto switch_case = parseSwitchCase();
        if (!switch_case) {
            report(switch_case.error());
            synchronize(SyncContext::SwitchCaseHeader, index != start_index);
            continue;
        }
        stmt.cases.push_back(std::move(*switch_case));
    }

    if (!check(TokenKind::RBrace)) {
        report(Diagnostic("expected '}' after switch", current().range));
        const auto end = stmt.cases.empty() ? lbrace->range.end
                                            : stmt.cases.back().range.end;
        return make_stmt(source_file.range(begin, end), std::move(stmt));
    }
    return make_stmt(source_file.range(begin, advance().range.end),
                     std::move(stmt));
}

auto Parser::parseSwitchCase() -> std::expected<ast::SwitchCase, Diagnostic> {
    ast::SwitchCase switch_case;
    SourceRange begin_range;
    if (match(TokenKind::KwDefault)) {
        switch_case.is_default = true;
        begin_range = previous().range;
    } else {
        auto case_token =
            expect(TokenKind::KwCase, "expected 'case' or 'default'");
        if (!case_token) {
            return std::unexpected(case_token.error());
        }
        begin_range = case_token->range;
        auto variant_name = parseIdentifier();
        if (!variant_name) {
            return std::unexpected(variant_name.error());
        }
        switch_case.variant_name = variant_name->text;
        switch_case.variant_name_range = variant_name->range;
        if (match(TokenKind::LParen)) {
            auto binding = parseIdentifier();
            if (!binding) {
                return std::unexpected(binding.error());
            }
            auto rparen =
                expect(TokenKind::RParen, "expected ')' after case binding");
            if (!rparen) {
                return std::unexpected(rparen.error());
            }
            switch_case.binding_name = binding->text;
            switch_case.binding_name_range = binding->range;
        }
    }
    switch_case.range = source_file.range(begin_range.begin, begin_range.begin);

    auto colon = expect(TokenKind::Colon, "expected ':' after case pattern");
    if (!colon) {
        return std::unexpected(colon.error());
    }

    auto body = std::make_unique<ast::Block>();
    body->range = source_file.range(colon->range.end, colon->range.end);
    while (!check(TokenKind::KwCase) && !check(TokenKind::KwDefault) &&
           !check(TokenKind::RBrace) && !isAtEnd()) {
        const auto start_index = index;
        auto stmt = parseStmt();
        if (!stmt) {
            report(stmt.error());
            synchronize(SyncContext::SwitchCaseBody, index != start_index);
            continue;
        }
        body->statements.push_back(std::move(*stmt));
    }
    body->range.end = body->statements.empty()
                          ? colon->range.end
                          : body->statements.back()->range.end;

    switch_case.range.end = body->range.end;
    switch_case.body = std::move(body);
    return switch_case;
}

auto Parser::parseReturnStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    ast::ReturnStmt stmt;
    if (!check(TokenKind::Semicolon)) {
        auto expr = parseExpr();
        if (!expr) {
            return std::unexpected(expr.error());
        }
        stmt.value = std::move(*expr);
    }
    auto semicolon = expect(TokenKind::Semicolon, "expected ';' after return");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }
    return make_stmt(source_file.range(begin, semicolon->range.end),
                     std::move(stmt));
}

auto Parser::parseDropStmt() -> std::expected<ast::StmtPtr, Diagnostic> {
    const auto begin = advance().range.begin;
    auto lparen = expect(TokenKind::LParen, "expected '(' after drop");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }
    auto value = parseExpr();
    if (!value) {
        return std::unexpected(value.error());
    }
    auto rparen = expect(TokenKind::RParen, "expected ')' after drop value");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }
    auto semicolon = expect(TokenKind::Semicolon, "expected ';' after drop");
    if (!semicolon) {
        return std::unexpected(semicolon.error());
    }

    ast::DropStmt stmt;
    stmt.value = std::move(*value);
    return make_stmt(source_file.range(begin, semicolon->range.end),
                     std::move(stmt));
}

auto Parser::parseExprOrAssignStmt()
    -> std::expected<ast::StmtPtr, Diagnostic> {
    return parseSimpleStmt(TokenKind::Semicolon,
                           "expected ';' after expression");
}

auto Parser::parseSimpleStmt(TokenKind terminator, std::string message)
    -> std::expected<ast::StmtPtr, Diagnostic> {
    auto expr = parseExpr();
    if (!expr) {
        return std::unexpected(expr.error());
    }

    if (match(TokenKind::Equal)) {
        auto rhs = parseExpr();
        if (!rhs) {
            return std::unexpected(rhs.error());
        }
        auto end_token = expect(terminator, std::move(message));
        if (!end_token) {
            return std::unexpected(end_token.error());
        }

        ast::AssignStmt stmt;
        stmt.target = std::move(*expr);
        stmt.value = std::move(*rhs);
        return make_stmt(
            source_file.range(stmt.target->range.begin, end_token->range.end),
            std::move(stmt));
    }

    if (match(TokenKind::PlusPlus) || match(TokenKind::MinusMinus)) {
        const auto is_increment = previous().kind == TokenKind::PlusPlus;
        auto end_token = expect(terminator, std::move(message));
        if (!end_token) {
            return std::unexpected(end_token.error());
        }
        ast::UpdateStmt stmt;
        stmt.target = std::move(*expr);
        stmt.is_increment = is_increment;
        return make_stmt(
            source_file.range(stmt.target->range.begin, end_token->range.end),
            std::move(stmt));
    }

    auto end_token = expect(terminator, std::move(message));
    if (!end_token) {
        return std::unexpected(end_token.error());
    }
    ast::ExprStmt stmt;
    stmt.expr = std::move(*expr);
    return make_stmt(
        source_file.range(stmt.expr->range.begin, end_token->range.end),
        std::move(stmt));
}

auto Parser::looksLikeVarDecl() -> bool {
    if (!(check(TokenKind::Ampersand) || check(TokenKind::LParen) ||
          check(TokenKind::LBracket) || check(TokenKind::KwConst) ||
          check(TokenKind::KwBool) || check(TokenKind::KwChar) ||
          check(TokenKind::KwF32) || check(TokenKind::KwF64) ||
          check(TokenKind::KwI8) || check(TokenKind::KwI16) ||
          check(TokenKind::KwI32) || check(TokenKind::KwI64) ||
          check(TokenKind::KwU8) || check(TokenKind::KwU16) ||
          check(TokenKind::KwU32) || check(TokenKind::KwU64) ||
          check(TokenKind::KwVoid) || check(TokenKind::Identifier))) {
        return false;
    }

    const auto saved_index = index;
    auto maybe_type = parseType();
    const auto looks_like_decl =
        maybe_type.has_value() && check(TokenKind::Identifier);
    index = saved_index;
    return looks_like_decl;
}

auto Parser::parseType() -> std::expected<ast::TypeSyntaxPtr, Diagnostic> {
    if (check(TokenKind::LBracket) && index + 1 < tokens.size() &&
        tokens[index + 1].kind == TokenKind::RBracket) {
        const auto begin = advance().range.begin;
        auto rbracket =
            expect(TokenKind::RBracket, "expected ']' in slice type");
        if (!rbracket) {
            return std::unexpected(rbracket.error());
        }
        auto element = parseType();
        if (!element) {
            return std::unexpected(element.error());
        }
        auto slice = std::make_unique<ast::TypeSyntax>();
        slice->range = source_file.range(begin, (*element)->range.end);
        slice->kind = ast::TypeSyntax::Kind::Slice;
        slice->element_type = std::move(*element);
        return slice;
    }

    auto parse_primary_type =
        [&]() -> std::expected<ast::TypeSyntaxPtr, Diagnostic> {
        if (match(TokenKind::LParen)) {
            const auto begin = previous().range.begin;
            auto inner = parseType();
            if (!inner) {
                return std::unexpected(inner.error());
            }
            auto rparen = expect(TokenKind::RParen, "expected ')' after type");
            if (!rparen) {
                return std::unexpected(rparen.error());
            }
            (*inner)->range = source_file.range(begin, rparen->range.end);
            return inner;
        }

        if (!(check(TokenKind::KwBool) || check(TokenKind::KwChar) ||
              check(TokenKind::KwF32) || check(TokenKind::KwF64) ||
              check(TokenKind::KwI8) || check(TokenKind::KwI16) ||
              check(TokenKind::KwI32) || check(TokenKind::KwI64) ||
              check(TokenKind::KwU8) || check(TokenKind::KwU16) ||
              check(TokenKind::KwU32) || check(TokenKind::KwU64) ||
              check(TokenKind::KwVoid) || check(TokenKind::Identifier))) {
            return std::unexpected(
                Diagnostic("expected type name", current().range));
        }

        const auto begin = current().range.begin;
        auto type = std::make_unique<ast::TypeSyntax>();
        type->range = source_file.range(begin, begin);
        type->kind = ast::TypeSyntax::Kind::Named;
        type->name = advance().text;
        type->name_range = previous().range;
        type->range.end = previous().range.end;
        if (check(TokenKind::Less)) {
            auto type_arguments = parseTypeArguments();
            if (!type_arguments) {
                return std::unexpected(type_arguments.error());
            }
            type->range.end = previous().range.end;
            type->type_arguments = std::move(*type_arguments);
        }
        return type;
    };

    auto parse_postfix_type =
        [&]() -> std::expected<ast::TypeSyntaxPtr, Diagnostic> {
        const auto is_const = match(TokenKind::KwConst);
        auto type = parse_primary_type();
        if (!type) {
            return std::unexpected(type.error());
        }
        (*type)->is_const = is_const;
        while (true) {
            if (match(TokenKind::Star)) {
                auto pointer = std::make_unique<ast::TypeSyntax>();
                pointer->range = source_file.range((*type)->range.begin,
                                                   previous().range.end);
                pointer->kind = ast::TypeSyntax::Kind::Pointer;
                pointer->element_type = std::move(*type);
                type = std::move(pointer);
                continue;
            }
            if (match(TokenKind::LBracket)) {
                auto size_token =
                    expect(TokenKind::Integer, "expected array size");
                if (!size_token) {
                    return std::unexpected(size_token.error());
                }
                auto rbracket = expect(TokenKind::RBracket,
                                       "expected ']' after array size");
                if (!rbracket) {
                    return std::unexpected(rbracket.error());
                }
                auto array = std::make_unique<ast::TypeSyntax>();
                array->range = source_file.range((*type)->range.begin,
                                                 rbracket->range.end);
                array->kind = ast::TypeSyntax::Kind::Array;
                array->element_type = std::move(*type);
                array->array_size = static_cast<std::uint64_t>(
                    std::strtoull(size_token->text.c_str(), nullptr, 10));
                type = std::move(array);
                continue;
            }
            break;
        }
        return type;
    };

    if (match(TokenKind::Ampersand)) {
        const auto begin = previous().range.begin;
        const auto is_mut = match(TokenKind::KwMut);
        auto element = check(TokenKind::LBracket) &&
                               index + 1 < tokens.size() &&
                               tokens[index + 1].kind == TokenKind::RBracket
                           ? parseType()
                           : parse_postfix_type();
        if (!element) {
            return std::unexpected(element.error());
        }
        auto borrow = std::make_unique<ast::TypeSyntax>();
        borrow->range = source_file.range(begin, (*element)->range.end);
        borrow->kind = ast::TypeSyntax::Kind::Borrow;
        borrow->element_type = std::move(*element);
        borrow->is_mut = is_mut;
        return borrow;
    }

    return parse_postfix_type();
}

auto Parser::parseExpr() -> std::expected<ast::ExprPtr, Diagnostic> {
    return parseLogicalOr();
}

auto Parser::parseLogicalOr() -> std::expected<ast::ExprPtr, Diagnostic> {
    static const auto ops = std::vector<std::pair<TokenKind, ast::BinaryOp>>{
        {TokenKind::OrOr, ast::BinaryOp::LogicalOr}};
    auto expr = parseLogicalAnd();
    if (!expr) {
        return std::unexpected(expr.error());
    }
    return binaryExprTail(std::move(*expr), ops, &Parser::parseLogicalAnd);
}

auto Parser::parseLogicalAnd() -> std::expected<ast::ExprPtr, Diagnostic> {
    static const auto ops = std::vector<std::pair<TokenKind, ast::BinaryOp>>{
        {TokenKind::AndAnd, ast::BinaryOp::LogicalAnd}};
    auto expr = parseEquality();
    if (!expr) {
        return std::unexpected(expr.error());
    }
    return binaryExprTail(std::move(*expr), ops, &Parser::parseEquality);
}

auto Parser::parseEquality() -> std::expected<ast::ExprPtr, Diagnostic> {
    static const auto ops = std::vector<std::pair<TokenKind, ast::BinaryOp>>{
        {TokenKind::EqualEqual, ast::BinaryOp::Equal},
        {TokenKind::BangEqual, ast::BinaryOp::NotEqual}};
    auto expr = parseRelational();
    if (!expr) {
        return std::unexpected(expr.error());
    }
    return binaryExprTail(std::move(*expr), ops, &Parser::parseRelational);
}

auto Parser::parseRelational() -> std::expected<ast::ExprPtr, Diagnostic> {
    static const auto ops = std::vector<std::pair<TokenKind, ast::BinaryOp>>{
        {TokenKind::Less, ast::BinaryOp::Less},
        {TokenKind::LessEqual, ast::BinaryOp::LessEqual},
        {TokenKind::Greater, ast::BinaryOp::Greater},
        {TokenKind::GreaterEqual, ast::BinaryOp::GreaterEqual}};
    auto expr = parseAdditive();
    if (!expr) {
        return std::unexpected(expr.error());
    }
    return binaryExprTail(std::move(*expr), ops, &Parser::parseAdditive);
}

auto Parser::parseAdditive() -> std::expected<ast::ExprPtr, Diagnostic> {
    static const auto ops = std::vector<std::pair<TokenKind, ast::BinaryOp>>{
        {TokenKind::Plus, ast::BinaryOp::Add},
        {TokenKind::Minus, ast::BinaryOp::Subtract}};
    auto expr = parseMultiplicative();
    if (!expr) {
        return std::unexpected(expr.error());
    }
    return binaryExprTail(std::move(*expr), ops, &Parser::parseMultiplicative);
}

auto Parser::parseMultiplicative() -> std::expected<ast::ExprPtr, Diagnostic> {
    static const auto ops = std::vector<std::pair<TokenKind, ast::BinaryOp>>{
        {TokenKind::Star, ast::BinaryOp::Multiply},
        {TokenKind::Slash, ast::BinaryOp::Divide},
        {TokenKind::Percent, ast::BinaryOp::Remainder}};
    auto expr = parseCast();
    if (!expr) {
        return std::unexpected(expr.error());
    }
    return binaryExprTail(std::move(*expr), ops, &Parser::parseCast);
}

auto Parser::parseCast() -> std::expected<ast::ExprPtr, Diagnostic> {
    auto expr = parseUnary();
    if (!expr) {
        return std::unexpected(expr.error());
    }

    while (match(TokenKind::KwAs)) {
        auto target_type = parseType();
        if (!target_type) {
            return std::unexpected(target_type.error());
        }
        expr = make_expr(
            source_file.range((*expr)->range.begin, (*target_type)->range.end),
            ast::CastExpr{.operand = std::move(*expr),
                          .target_type = std::move(*target_type),
                          .cast_kind = ast::CastKind::None});
    }

    return expr;
}

auto Parser::parseUnary() -> std::expected<ast::ExprPtr, Diagnostic> {
    if (match(TokenKind::Minus)) {
        auto op = previous();
        auto operand = parseUnary();
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return make_expr(
            source_file.range(op.range.begin, (*operand)->range.end),
            ast::UnaryExpr{.op = ast::UnaryOp::Negate,
                           .operand = std::move(*operand)});
    }
    if (match(TokenKind::Bang)) {
        auto op = previous();
        auto operand = parseUnary();
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return make_expr(
            source_file.range(op.range.begin, (*operand)->range.end),
            ast::UnaryExpr{.op = ast::UnaryOp::LogicalNot,
                           .operand = std::move(*operand)});
    }
    if (match(TokenKind::Star)) {
        auto op = previous();
        auto operand = parseUnary();
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return make_expr(
            source_file.range(op.range.begin, (*operand)->range.end),
            ast::UnaryExpr{.op = ast::UnaryOp::Dereference,
                           .operand = std::move(*operand)});
    }
    if (match(TokenKind::KwMove)) {
        auto op = previous();
        auto operand = parseUnary();
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return make_expr(
            source_file.range(op.range.begin, (*operand)->range.end),
            ast::UnaryExpr{.op = ast::UnaryOp::Move,
                           .operand = std::move(*operand)});
    }
    if (match(TokenKind::Ampersand)) {
        auto op = previous();
        const auto is_mut = match(TokenKind::KwMut);
        auto operand = parseUnary();
        if (!operand) {
            return std::unexpected(operand.error());
        }
        return make_expr(
            source_file.range(op.range.begin, (*operand)->range.end),
            ast::UnaryExpr{.op = is_mut ? ast::UnaryOp::BorrowMut
                                        : ast::UnaryOp::Borrow,
                           .operand = std::move(*operand)});
    }
    return parsePostfix();
}

auto Parser::parsePostfix() -> std::expected<ast::ExprPtr, Diagnostic> {
    auto expr = parsePrimary();
    if (!expr) {
        return std::unexpected(expr.error());
    }

    while (true) {
        if (match(TokenKind::LParen)) {
            ast::CallExpr call;
            if (const auto* name = std::get_if<ast::NameExpr>(&(*expr)->node);
                name != nullptr) {
                call.callee = name->name;
                call.callee_range = name->name_range;
            } else {
                return std::unexpected(Diagnostic(
                    "calls currently require a direct function or variant name",
                    (*expr)->range));
            }

            if (!check(TokenKind::RParen)) {
                while (true) {
                    auto argument = parseExpr();
                    if (!argument) {
                        return std::unexpected(argument.error());
                    }
                    call.arguments.push_back(std::move(*argument));
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                }
            }
            auto rparen =
                expect(TokenKind::RParen, "expected ')' after arguments");
            if (!rparen) {
                return std::unexpected(rparen.error());
            }
            expr = make_expr(
                source_file.range((*expr)->range.begin, rparen->range.end),
                std::move(call));
            continue;
        }

        if (match(TokenKind::Dot)) {
            auto member = parseIdentifier();
            if (!member) {
                return std::unexpected(member.error());
            }
            ast::MemberExpr member_expr;
            member_expr.base = std::move(*expr);
            member_expr.field_name = member->text;
            member_expr.field_range = member->range;
            expr = make_expr(source_file.range(member_expr.base->range.begin,
                                               member->range.end),
                             std::move(member_expr));
            continue;
        }

        if (match(TokenKind::LBracket)) {
            auto index_expr = parseExpr();
            if (!index_expr) {
                return std::unexpected(index_expr.error());
            }
            auto rbracket =
                expect(TokenKind::RBracket, "expected ']' after index");
            if (!rbracket) {
                return std::unexpected(rbracket.error());
            }
            expr = make_expr(
                source_file.range((*expr)->range.begin, rbracket->range.end),
                ast::IndexExpr{.base = std::move(*expr),
                               .index = std::move(*index_expr)});
            continue;
        }

        break;
    }

    return expr;
}

auto Parser::parsePrimary() -> std::expected<ast::ExprPtr, Diagnostic> {
    if (match(TokenKind::Integer)) {
        return make_expr(previous().range,
                         ast::IntegerLiteralExpr{std::strtoll(
                             previous().text.c_str(), nullptr, 10)});
    }
    if (match(TokenKind::Float)) {
        return make_expr(previous().range,
                         ast::FloatLiteralExpr{
                             std::strtod(previous().text.c_str(), nullptr)});
    }
    if (match(TokenKind::Character)) {
        const auto text = previous().text;
        char value = '\0';
        if (text[1] == '\\') {
            switch (text[2]) {
            case '\'':
                value = '\'';
                break;
            case '\\':
                value = '\\';
                break;
            case 'n':
                value = '\n';
                break;
            case 'r':
                value = '\r';
                break;
            case 't':
                value = '\t';
                break;
            case '0':
                value = '\0';
                break;
            default:
                value = text[2];
                break;
            }
        } else {
            value = text[1];
        }
        return make_expr(previous().range, ast::CharLiteralExpr{value});
    }
    if (match(TokenKind::String)) {
        auto decoded = decode_string_literal(previous().text);
        if (!decoded) {
            return std::unexpected(
                Diagnostic(std::move(decoded.error()), previous().range));
        }
        return make_expr(previous().range,
                         ast::StringLiteralExpr{std::move(*decoded)});
    }
    if (match(TokenKind::True)) {
        return make_expr(previous().range, ast::BoolLiteralExpr{true});
    }
    if (match(TokenKind::False)) {
        return make_expr(previous().range, ast::BoolLiteralExpr{false});
    }
    if (match(TokenKind::KwSizeof)) {
        const auto begin = previous().range.begin;
        auto lparen = expect(TokenKind::LParen, "expected '(' after sizeof");
        if (!lparen) {
            return std::unexpected(lparen.error());
        }
        auto type = parseType();
        if (!type) {
            return std::unexpected(type.error());
        }
        auto rparen = expect(TokenKind::RParen, "expected ')' after sizeof");
        if (!rparen) {
            return std::unexpected(rparen.error());
        }
        return make_expr(
            source_file.range(begin, rparen->range.end),
            ast::SizeofExpr{.type = std::move(*type), .operand_type = nullptr});
    }
    if (match(TokenKind::Identifier)) {
        ast::NameExpr name_expr;
        name_expr.name = previous().text;
        name_expr.name_range = previous().range;
        return make_expr(previous().range, std::move(name_expr));
    }
    if (match(TokenKind::LBrace)) {
        auto init = ast::InitListExpr{};
        const auto begin = previous().range.begin;
        if (!check(TokenKind::RBrace)) {
            while (true) {
                auto element = parseExpr();
                if (!element) {
                    return std::unexpected(element.error());
                }
                init.elements.push_back(std::move(*element));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
        }
        auto rbrace =
            expect(TokenKind::RBrace, "expected '}' after initializer list");
        if (!rbrace) {
            return std::unexpected(rbrace.error());
        }
        return make_expr(source_file.range(begin, rbrace->range.end),
                         std::move(init));
    }
    if (match(TokenKind::LBracket)) {
        ast::ArrayLiteralExpr array;
        const auto begin = previous().range.begin;
        if (!check(TokenKind::RBracket)) {
            while (true) {
                auto element = parseExpr();
                if (!element) {
                    return std::unexpected(element.error());
                }
                array.elements.push_back(std::move(*element));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
        }
        auto rbracket =
            expect(TokenKind::RBracket, "expected ']' after array literal");
        if (!rbracket) {
            return std::unexpected(rbracket.error());
        }
        return make_expr(source_file.range(begin, rbracket->range.end),
                         std::move(array));
    }
    if (match(TokenKind::LParen)) {
        auto lparen = previous();
        auto expr = parseExpr();
        if (!expr) {
            return std::unexpected(expr.error());
        }
        auto rparen =
            expect(TokenKind::RParen, "expected ')' after expression");
        if (!rparen) {
            return std::unexpected(rparen.error());
        }
        (*expr)->range =
            source_file.range(lparen.range.begin, rparen->range.end);
        return expr;
    }

    return std::unexpected(Diagnostic("expected expression", current().range));
}

auto Parser::parseIdentifier() -> std::expected<Token, Diagnostic> {
    return expect(TokenKind::Identifier, "expected identifier");
}

auto Parser::parseInterfaceName() -> std::expected<Token, Diagnostic> {
    if (check(TokenKind::KwDrop)) {
        return advance();
    }
    return parseIdentifier();
}

auto Parser::parseTypeArguments()
    -> std::expected<std::vector<ast::TypeSyntaxPtr>, Diagnostic> {
    if (!match(TokenKind::Less)) {
        return std::unexpected(
            Diagnostic("expected '<' before type arguments", current().range));
    }

    std::vector<ast::TypeSyntaxPtr> type_arguments;
    while (true) {
        auto type_argument = parseType();
        if (!type_argument) {
            return std::unexpected(type_argument.error());
        }
        type_arguments.push_back(std::move(*type_argument));
        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    auto greater =
        expect(TokenKind::Greater, "expected '>' after type arguments");
    if (!greater) {
        return std::unexpected(greater.error());
    }
    return type_arguments;
}

auto Parser::parseTypeParameters()
    -> std::expected<std::vector<Token>, Diagnostic> {
    if (!match(TokenKind::Less)) {
        return std::unexpected(
            Diagnostic("expected '<' before type parameters", current().range));
    }

    std::vector<Token> type_parameters;
    while (true) {
        auto parameter = parseIdentifier();
        if (!parameter) {
            return std::unexpected(parameter.error());
        }
        type_parameters.push_back(*parameter);
        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    auto greater =
        expect(TokenKind::Greater, "expected '>' after type parameters");
    if (!greater) {
        return std::unexpected(greater.error());
    }
    return type_parameters;
}

auto Parser::parseDependencyPath()
    -> std::expected<ast::DependencyPath, Diagnostic> {
    ast::DependencyPath path;

    Token root_token{};
    if (check(TokenKind::KwReturn)) {
        root_token = advance();
        path.is_return = true;
    } else {
        auto identifier = parseIdentifier();
        if (!identifier) {
            return std::unexpected(identifier.error());
        }
        root_token = *identifier;
        path.root_name = root_token.text;
    }

    path.root_range = root_token.range;
    path.range = root_token.range;
    while (match(TokenKind::Dot)) {
        auto segment = parseIdentifier();
        if (!segment) {
            return std::unexpected(segment.error());
        }
        path.segments.push_back(ast::DependencyPathSegment{
            .name = segment->text,
            .range = segment->range,
        });
        path.range.end = segment->range.end;
    }
    return path;
}

auto Parser::parseDependsClause(ast::FunctionDecl& decl)
    -> std::expected<void, Diagnostic> {
    if (!match(TokenKind::KwDepends)) {
        return {};
    }

    auto lparen = expect(TokenKind::LParen, "expected '(' after depends");
    if (!lparen) {
        return std::unexpected(lparen.error());
    }
    while (true) {
        auto target = parseDependencyPath();
        if (!target) {
            return std::unexpected(target.error());
        }
        auto on_token =
            expect(TokenKind::KwOn, "expected 'on' in depends clause");
        if (!on_token) {
            return std::unexpected(on_token.error());
        }
        auto source = parseDependencyPath();
        if (!source) {
            return std::unexpected(source.error());
        }

        auto dependency = ast::ReturnDependency{
            .target = std::move(*target),
            .source = std::move(*source),
            .range =
                source_file.range((*target).range.begin, (*source).range.end),
        };
        decl.declared_return_dependencies.push_back(dependency);
        decl.return_dependencies.push_back(std::move(dependency));

        if (!match(TokenKind::Comma)) {
            break;
        }
    }

    auto rparen =
        expect(TokenKind::RParen, "expected ')' after depends clause");
    if (!rparen) {
        return std::unexpected(rparen.error());
    }
    return {};
}

auto Parser::current() const -> const Token& { return tokens[index]; }

auto Parser::previous() const -> const Token& { return tokens[index - 1]; }

auto Parser::isAtEnd() const -> bool {
    return current().kind == TokenKind::EndOfFile;
}

auto Parser::check(TokenKind kind) const -> bool {
    return current().kind == kind;
}

auto Parser::advance() -> const Token& {
    if (!isAtEnd()) {
        ++index;
    }
    return previous();
}

auto Parser::match(TokenKind kind) -> bool {
    if (!check(kind)) {
        return false;
    }
    advance();
    return true;
}

auto Parser::expect(TokenKind kind, std::string message)
    -> std::expected<Token, Diagnostic> {
    if (!check(kind)) {
        return std::unexpected(Diagnostic(std::move(message), current().range));
    }
    return advance();
}

auto Parser::report(Diagnostic diagnostic) -> void {
    diagnostics.push_back(std::move(diagnostic));
}

auto Parser::synchronize(SyncContext context, bool progressed) -> void {
    struct Nesting {
        std::size_t paren = 0;
        std::size_t brace = 0;
        std::size_t bracket = 0;
    };

    auto consume = [&](Nesting& nesting) {
        if (isAtEnd()) {
            return;
        }

        switch (current().kind) {
        case TokenKind::LParen:
            ++nesting.paren;
            break;
        case TokenKind::RParen:
            if (nesting.paren > 0) {
                --nesting.paren;
            }
            break;
        case TokenKind::LBrace:
            ++nesting.brace;
            break;
        case TokenKind::RBrace:
            if (nesting.brace > 0) {
                --nesting.brace;
            }
            break;
        case TokenKind::LBracket:
            ++nesting.bracket;
            break;
        case TokenKind::RBracket:
            if (nesting.bracket > 0) {
                --nesting.bracket;
            }
            break;
        default:
            break;
        }
        advance();
    };

    if (isAtEnd()) {
        return;
    }

    Nesting nesting;
    if (!progressed) {
        if (context == SyncContext::ParameterList &&
            isSyncBoundary(context, current().kind)) {
            return;
        }
        consume(nesting);
    }

    while (!isAtEnd()) {
        if (nesting.paren == 0 && nesting.brace == 0 && nesting.bracket == 0) {
            if (isSyncBoundary(context, current().kind)) {
                return;
            }
            if (index > 0 && previous().kind == TokenKind::Semicolon &&
                context != SyncContext::ParameterList) {
                return;
            }
        }
        consume(nesting);
    }
}

auto Parser::canStartType(TokenKind kind) const -> bool {
    switch (kind) {
    case TokenKind::Ampersand:
    case TokenKind::LBracket:
    case TokenKind::LParen:
    case TokenKind::KwConst:
    case TokenKind::KwBool:
    case TokenKind::KwChar:
    case TokenKind::KwF32:
    case TokenKind::KwF64:
    case TokenKind::KwI8:
    case TokenKind::KwI16:
    case TokenKind::KwI32:
    case TokenKind::KwI64:
    case TokenKind::KwU8:
    case TokenKind::KwU16:
    case TokenKind::KwU32:
    case TokenKind::KwU64:
    case TokenKind::KwVoid:
    case TokenKind::Identifier:
        return true;
    default:
        return false;
    }
}

auto Parser::canStartExpr(TokenKind kind) const -> bool {
    switch (kind) {
    case TokenKind::Integer:
    case TokenKind::Float:
    case TokenKind::Character:
    case TokenKind::String:
    case TokenKind::True:
    case TokenKind::False:
    case TokenKind::Identifier:
    case TokenKind::LBrace:
    case TokenKind::LBracket:
    case TokenKind::LParen:
    case TokenKind::KwSizeof:
    case TokenKind::Minus:
    case TokenKind::Bang:
    case TokenKind::Star:
    case TokenKind::Ampersand:
    case TokenKind::KwMove:
        return true;
    default:
        return false;
    }
}

auto Parser::canStartStmt(TokenKind kind) const -> bool {
    switch (kind) {
    case TokenKind::LBrace:
    case TokenKind::KwIf:
    case TokenKind::KwWhile:
    case TokenKind::KwFor:
    case TokenKind::KwBreak:
    case TokenKind::KwContinue:
    case TokenKind::KwUnchecked:
    case TokenKind::KwSwitch:
    case TokenKind::KwReturn:
    case TokenKind::KwDrop:
        return true;
    default:
        break;
    }
    return canStartType(kind) || canStartExpr(kind);
}

auto Parser::canStartTopLevelDecl(TokenKind kind) const -> bool {
    switch (kind) {
    case TokenKind::KwImport:
    case TokenKind::KwExport:
    case TokenKind::KwExtern:
    case TokenKind::KwImpl:
    case TokenKind::KwInterface:
    case TokenKind::KwStruct:
    case TokenKind::KwEnum:
        return true;
    default:
        return canStartType(kind);
    }
}

auto Parser::isSyncBoundary(SyncContext context, TokenKind kind) const -> bool {
    switch (context) {
    case SyncContext::TopLevel:
        return canStartTopLevelDecl(kind) || kind == TokenKind::EndOfFile;
    case SyncContext::Block:
        return kind == TokenKind::RBrace || canStartStmt(kind);
    case SyncContext::SwitchCaseHeader:
        return kind == TokenKind::KwCase || kind == TokenKind::KwDefault ||
               kind == TokenKind::RBrace || kind == TokenKind::EndOfFile;
    case SyncContext::SwitchCaseBody:
        return kind == TokenKind::KwCase || kind == TokenKind::KwDefault ||
               kind == TokenKind::RBrace || canStartStmt(kind);
    case SyncContext::StructBody:
        return kind == TokenKind::RBrace || kind == TokenKind::KwImpl ||
               canStartType(kind);
    case SyncContext::EnumBody:
        return kind == TokenKind::RBrace || kind == TokenKind::KwImpl ||
               kind == TokenKind::Identifier;
    case SyncContext::ExternBlock:
        return kind == TokenKind::RBrace || canStartType(kind);
    case SyncContext::ParameterList:
        return kind == TokenKind::RParen || kind == TokenKind::Comma ||
               kind == TokenKind::LBrace;
    }
    return false;
}

auto Parser::binaryExprTail(
    ast::ExprPtr expr,
    const std::vector<std::pair<TokenKind, ast::BinaryOp>>& ops,
    std::expected<ast::ExprPtr, Diagnostic> (Parser::*subparser)())
    -> std::expected<ast::ExprPtr, Diagnostic> {
    while (true) {
        std::optional<ast::BinaryOp> op;
        for (const auto& [token_kind, binary_op] : ops) {
            if (match(token_kind)) {
                op = binary_op;
                break;
            }
        }
        if (!op.has_value()) {
            break;
        }

        auto rhs = (this->*subparser)();
        if (!rhs) {
            return std::unexpected(rhs.error());
        }
        const auto begin = expr->range.begin;
        const auto end = (*rhs)->range.end;
        expr = make_expr(source_file.range(begin, end),
                         ast::BinaryExpr{.op = *op,
                                         .lhs = std::move(expr),
                                         .rhs = std::move(*rhs)});
    }
    return expr;
}

} // namespace cyan

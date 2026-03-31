#include "cyan/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace cyan {

namespace {

auto keyword_kind(std::string_view text) -> std::optional<TokenKind> {
    static const std::unordered_map<std::string_view, TokenKind> keywords = {
        {"as", TokenKind::KwAs},
        {"bool", TokenKind::KwBool},
        {"break", TokenKind::KwBreak},
        {"case", TokenKind::KwCase},
        {"char", TokenKind::KwChar},
        {"const", TokenKind::KwConst},
        {"continue", TokenKind::KwContinue},
        {"depends", TokenKind::KwDepends},
        {"default", TokenKind::KwDefault},
        {"drop", TokenKind::KwDrop},
        {"else", TokenKind::KwElse},
        {"enum", TokenKind::KwEnum},
        {"export", TokenKind::KwExport},
        {"extern", TokenKind::KwExtern},
        {"false", TokenKind::False},
        {"f32", TokenKind::KwF32},
        {"f64", TokenKind::KwF64},
        {"for", TokenKind::KwFor},
        {"impl", TokenKind::KwImpl},
        {"if", TokenKind::KwIf},
        {"i8", TokenKind::KwI8},
        {"i16", TokenKind::KwI16},
        {"i32", TokenKind::KwI32},
        {"i64", TokenKind::KwI64},
        {"i128", TokenKind::KwI128},
        {"import", TokenKind::KwImport},
        {"interface", TokenKind::KwInterface},
        {"move", TokenKind::KwMove},
        {"mut", TokenKind::KwMut},
        {"on", TokenKind::KwOn},
        {"return", TokenKind::KwReturn},
        {"sizeof", TokenKind::KwSizeof},
        {"struct", TokenKind::KwStruct},
        {"switch", TokenKind::KwSwitch},
        {"true", TokenKind::True},
        {"u8", TokenKind::KwU8},
        {"u16", TokenKind::KwU16},
        {"u32", TokenKind::KwU32},
        {"u64", TokenKind::KwU64},
        {"u128", TokenKind::KwU128},
        {"unchecked", TokenKind::KwUnchecked},
        {"void", TokenKind::KwVoid},
        {"while", TokenKind::KwWhile},
    };

    if (const auto it = keywords.find(text); it != keywords.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto decode_char_literal(std::string_view text)
    -> std::expected<char, std::string> {
    if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
        return std::unexpected("invalid character literal");
    }

    const auto body = text.substr(1, text.size() - 2);
    if (body.empty()) {
        return std::unexpected("empty character literal");
    }
    if (body.front() != '\\') {
        if (body.size() != 1) {
            return std::unexpected(
                "character literal must contain one character");
        }
        return static_cast<char>(body.front());
    }

    if (body.size() != 2) {
        return std::unexpected("unsupported character escape");
    }

    switch (body[1]) {
    case '\'':
        return '\'';
    case '\\':
        return '\\';
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case '0':
        return '\0';
    default:
        return std::unexpected("unsupported character escape");
    }
}

} // namespace

Lexer::Lexer(const SourceFile& source)
    : source_file(source), source_text(source.text()) {}

auto Lexer::lexAll() -> std::expected<std::vector<Token>, Diagnostic> {
    std::vector<Token> tokens;
    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) {
            break;
        }

        const auto ch = current();
        if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
            tokens.push_back(lexIdentifierOrKeyword());
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            tokens.push_back(lexNumber());
            continue;
        }
        if (ch == '\'') {
            auto token = lexCharacter();
            if (!token) {
                return std::unexpected(token.error());
            }
            tokens.push_back(std::move(*token));
            continue;
        }
        if (ch == '"') {
            auto token = lexString();
            if (!token) {
                return std::unexpected(token.error());
            }
            tokens.push_back(std::move(*token));
            continue;
        }

        auto token = lexPunctuation();
        if (!token) {
            return std::unexpected(token.error());
        }
        tokens.push_back(std::move(*token));
    }

    tokens.push_back(Token{.kind = TokenKind::EndOfFile,
                           .text = "",
                           .range = source_file.range(cursor, cursor)});
    return tokens;
}

auto Lexer::current() const -> char { return source_text[cursor]; }

auto Lexer::peek() const -> char {
    if (cursor + 1 >= source_text.size()) {
        return '\0';
    }
    return source_text[cursor + 1];
}

auto Lexer::isAtEnd() const -> bool { return cursor >= source_text.size(); }

auto Lexer::advance() -> char { return source_text[cursor++]; }

auto Lexer::skipWhitespace() -> void {
    while (!isAtEnd()) {
        const auto ch = current();
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            advance();
            continue;
        }

        if (ch == '/' && peek() == '/') {
            advance();
            advance();
            while (!isAtEnd() && current() != '\n') {
                advance();
            }
            continue;
        }

        break;
    }
}

auto Lexer::lexIdentifierOrKeyword() -> Token {
    const auto begin = cursor;
    while (!isAtEnd()) {
        const auto ch = current();
        if (std::isalnum(static_cast<unsigned char>(ch)) == 0 && ch != '_') {
            break;
        }
        advance();
    }

    const auto text = source_file.slice({.begin = begin, .end = cursor});
    const auto kind = keyword_kind(text).value_or(TokenKind::Identifier);
    return Token{.kind = kind,
                 .text = std::string(text),
                 .range = source_file.range(begin, cursor)};
}

auto Lexer::lexNumber() -> Token {
    const auto begin = cursor;
    while (!isAtEnd() &&
           std::isdigit(static_cast<unsigned char>(current())) != 0) {
        advance();
    }

    auto kind = TokenKind::Integer;
    if (!isAtEnd() && current() == '.' && cursor + 1 < source_text.size() &&
        std::isdigit(static_cast<unsigned char>(source_text[cursor + 1])) !=
            0) {
        kind = TokenKind::Float;
        advance();
        while (!isAtEnd() &&
               std::isdigit(static_cast<unsigned char>(current())) != 0) {
            advance();
        }
    }

    return Token{.kind = kind,
                 .text = std::string(
                     source_file.slice(source_file.range(begin, cursor))),
                 .range = source_file.range(begin, cursor)};
}

auto Lexer::lexCharacter() -> std::expected<Token, Diagnostic> {
    const auto begin = cursor;
    advance();
    if (isAtEnd()) {
        return std::unexpected(Diagnostic("unterminated character literal",
                                          source_file.range(begin, cursor)));
    }

    if (current() == '\\') {
        advance();
        if (isAtEnd()) {
            return std::unexpected(
                Diagnostic("unterminated character literal",
                           source_file.range(begin, cursor)));
        }
    }

    advance();
    if (isAtEnd() || current() != '\'') {
        return std::unexpected(Diagnostic("unterminated character literal",
                                          source_file.range(begin, cursor)));
    }
    advance();

    const auto text = source_file.slice({begin, cursor});
    auto decoded = decode_char_literal(text);
    if (!decoded) {
        return std::unexpected(Diagnostic(std::move(decoded.error()),
                                          source_file.range(begin, cursor)));
    }
    return Token{.kind = TokenKind::Character,
                 .text = std::string(text),
                 .range = {.begin = begin, .end = cursor}};
}

auto Lexer::lexString() -> std::expected<Token, Diagnostic> {
    const auto begin = cursor;
    advance();

    while (!isAtEnd() && current() != '"') {
        if (current() == '\n') {
            return std::unexpected(
                Diagnostic("unterminated string literal",
                           source_file.range(begin, cursor)));
        }
        if (current() == '\\') {
            advance();
            if (isAtEnd()) {
                return std::unexpected(
                    Diagnostic("unterminated string literal",
                               source_file.range(begin, cursor)));
            }
        }
        advance();
    }

    if (isAtEnd() || current() != '"') {
        return std::unexpected(Diagnostic("unterminated string literal",
                                          source_file.range(begin, cursor)));
    }
    advance();
    return Token{.kind = TokenKind::String,
                 .text = std::string(
                     source_file.slice(source_file.range(begin, cursor))),
                 .range = source_file.range(begin, cursor)};
}

auto Lexer::lexPunctuation() -> std::expected<Token, Diagnostic> {
    const auto begin = cursor;
    const auto ch = advance();

    auto make = [&](TokenKind kind) -> Token {
        return Token{.kind = kind,
                     .text = std::string(
                         source_file.slice(source_file.range(begin, cursor))),
                     .range = source_file.range(begin, cursor)};
    };

    switch (ch) {
    case '&':
        if (!isAtEnd() && current() == '&') {
            advance();
            return make(TokenKind::AndAnd);
        }
        return make(TokenKind::Ampersand);
    case '*':
        return make(TokenKind::Star);
    case '.':
        return make(TokenKind::Dot);
    case ',':
        return make(TokenKind::Comma);
    case ';':
        return make(TokenKind::Semicolon);
    case ':':
        return make(TokenKind::Colon);
    case '(':
        return make(TokenKind::LParen);
    case ')':
        return make(TokenKind::RParen);
    case '{':
        return make(TokenKind::LBrace);
    case '}':
        return make(TokenKind::RBrace);
    case '[':
        return make(TokenKind::LBracket);
    case ']':
        return make(TokenKind::RBracket);
    case '+':
        if (!isAtEnd() && current() == '+') {
            advance();
            return make(TokenKind::PlusPlus);
        }
        return make(TokenKind::Plus);
    case '-':
        if (!isAtEnd() && current() == '-') {
            advance();
            return make(TokenKind::MinusMinus);
        }
        return make(TokenKind::Minus);
    case '/':
        return make(TokenKind::Slash);
    case '%':
        return make(TokenKind::Percent);
    case '|':
        if (!isAtEnd() && current() == '|') {
            advance();
            return make(TokenKind::OrOr);
        }
        return make(TokenKind::Pipe);
    case '^':
        return make(TokenKind::Caret);
    case '~':
        return make(TokenKind::Tilde);
    case '=':
        if (!isAtEnd() && current() == '=') {
            advance();
            return make(TokenKind::EqualEqual);
        }
        return make(TokenKind::Equal);
    case '!':
        if (!isAtEnd() && current() == '=') {
            advance();
            return make(TokenKind::BangEqual);
        }
        return make(TokenKind::Bang);
    case '<':
        if (!isAtEnd() && current() == '=') {
            advance();
            return make(TokenKind::LessEqual);
        }
        return make(TokenKind::Less);
    case '>':
        if (!isAtEnd() && current() == '=') {
            advance();
            return make(TokenKind::GreaterEqual);
        }
        return make(TokenKind::Greater);
    default:
        break;
    }

    return std::unexpected(
        Diagnostic("unexpected character", source_file.range(begin, cursor)));
}

} // namespace cyan

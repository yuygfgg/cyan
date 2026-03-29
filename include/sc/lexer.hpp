#pragma once

#include "sc/diagnostic.hpp"
#include "sc/source.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace sc {

enum class TokenKind : std::uint8_t {
    EndOfFile,
    Identifier,
    Integer,
    Float,
    Character,
    String,
    True,
    False,
    KwAs,
    KwBool,
    KwChar,
    KwConst,
    KwCase,
    KwBreak,
    KwContinue,
    KwDrop,
    KwDepends,
    KwDefault,
    KwElse,
    KwEnum,
    KwExport,
    KwExtern,
    KwFloat,
    KwFor,
    KwImpl,
    KwIf,
    KwImport,
    KwInt,
    KwInterface,
    KwMove,
    KwMut,
    KwOn,
    KwReturn,
    KwSizeof,
    KwStruct,
    KwSwitch,
    KwUnchecked,
    KwVoid,
    KwWhile,
    Colon,
    Ampersand,
    Star,
    Dot,
    Comma,
    Semicolon,
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Equal,
    EqualEqual,
    Bang,
    BangEqual,
    Plus,
    PlusPlus,
    Minus,
    MinusMinus,
    Slash,
    Percent,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AndAnd,
    OrOr,
};

struct Token {
    TokenKind kind = TokenKind::EndOfFile;
    std::string text;
    SourceRange range;
};

class Lexer {
  public:
    explicit Lexer(const SourceFile& source);

    auto lexAll() -> std::expected<std::vector<Token>, Diagnostic>;

  private:
    [[nodiscard]] auto current() const -> char;
    [[nodiscard]] auto peek() const -> char;
    [[nodiscard]] auto isAtEnd() const -> bool;
    auto advance() -> char;
    auto skipWhitespace() -> void;
    auto lexIdentifierOrKeyword() -> Token;
    auto lexNumber() -> Token;
    auto lexCharacter() -> std::expected<Token, Diagnostic>;
    auto lexString() -> std::expected<Token, Diagnostic>;
    auto lexPunctuation() -> std::expected<Token, Diagnostic>;

    const SourceFile& source_file;
    std::string_view source_text;
    std::size_t cursor = 0;
};

} // namespace sc

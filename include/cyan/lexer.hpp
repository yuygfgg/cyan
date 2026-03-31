#pragma once

#include "cyan/diagnostic.hpp"
#include "cyan/source.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace cyan {

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
    KwF32,
    KwF64,
    KwFor,
    KwImpl,
    KwIf,
    KwI8,
    KwI16,
    KwI32,
    KwI64,
    KwImport,
    KwInterface,
    KwMove,
    KwMut,
    KwOn,
    KwReturn,
    KwSizeof,
    KwStruct,
    KwSwitch,
    KwU8,
    KwU16,
    KwU32,
    KwU64,
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

} // namespace cyan

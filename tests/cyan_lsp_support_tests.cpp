#include "cyan/diagnostic.hpp"
#include "cyan/lexer.hpp"
#include "cyan/lsp_support.hpp"
#include "cyan/parser.hpp"
#include "cyan/sema.hpp"
#include "cyan/source.hpp"
#include "cyan/type.hpp"

#include "glaze/glaze.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lsp_test {

// NOLINTBEGIN(readability-identifier-naming)

struct MessageEnvelope {
    std::string jsonrpc;
    std::string method;
    std::optional<glz::raw_json> params;
    std::optional<glz::raw_json> id;
    std::optional<glz::raw_json> result;
    std::optional<glz::raw_json> error;
};

struct Position {
    std::size_t line = 0;
    std::size_t character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Diagnostic {
    Range range;
    int severity = 0;
    std::string source;
    std::string message;
};

struct PublishDiagnosticsParams {
    std::string uri;
    std::vector<Diagnostic> diagnostics;
    std::optional<int> version;
};

struct TextDocumentSyncOptions {
    bool openClose = false;
    int change = 0;
};

struct SemanticTokensLegend {
    std::vector<std::string> tokenTypes;
    std::vector<std::string> tokenModifiers;
};

struct SemanticTokensOptions {
    SemanticTokensLegend legend;
    bool full = false;
};

struct ServerCapabilities {
    TextDocumentSyncOptions textDocumentSync;
    bool hoverProvider = false;
    bool definitionProvider = false;
    bool renameProvider = false;
    SemanticTokensOptions semanticTokensProvider;
};

struct InitializeResult {
    ServerCapabilities capabilities;
};

struct MarkupContent {
    std::string kind;
    std::string value;
};

struct Hover {
    MarkupContent contents;
    std::optional<Range> range;
};

struct Location {
    std::string uri;
    Range range;
};

struct TextEdit {
    Range range;
    std::string newText;
};

struct WorkspaceEdit {
    std::unordered_map<std::string, std::vector<TextEdit>> changes;
};

struct SemanticTokens {
    std::vector<std::uint32_t> data;
};

struct BuiltinSourceResult {
    std::string text;
};

// NOLINTEND(readability-identifier-naming)

} // namespace lsp_test

namespace {

constexpr std::string_view FIXTURE_SOURCE = R"(enum Option<T> {
    None,
    Some(T),
};

struct Pair {
    i64 value;
};

i64 unwrap(Option<i64> value) {
    Pair pair = {1};
    switch (move value) {
        case None:
            return pair.value;
        case Some(payload):
            return payload + pair.value;
    }
}
)";

constexpr std::string_view INTERFACE_FIXTURE_SOURCE =
    R"(interface<T> i64 measure(&T value);

struct Box {
    i64 value;
};

impl measure(&Box box) {
    return box.value;
}

i64 main() {
    Box box = {7};
    return measure(box);
}
)";

constexpr std::string_view DEPENDS_SHORTHAND_FIXTURE_SOURCE = R"(struct Pair {
    []const char left;
    []const char right;
};

Pair pair([]const char text) depends(return on text) {
    return {text, text};
}
)";

constexpr std::string_view BUILTIN_IMPORT_FIXTURE_SOURCE =
    R"(import /std.mem as mem;

i64 main() {
    char[4] text = "abc";
    mem.memcpy(&mut text[0], &text[0], 4);
    return 0;
}
)";

constexpr std::string_view INTERFACE_ALIAS_FIXTURE_SOURCE =
    R"(interface<T> i64 left(&T value);
interface<T> i64 right(&T value);
interface both = left + right;
interface first = left;
interface second = left;

struct Pair {
    i64 lhs;
    i64 rhs;
};

impl left(&Pair value) {
    return value.lhs;
}

impl right(&Pair value) {
    return value.rhs;
}

i64 use_both(&both value) {
    return left(value) + right(value);
}

i64 use_second(&second value) {
    return left(value);
}

i64 main() {
    Pair pair = {1, 2};
    return use_both(pair) + use_second(pair) - 4;
}
)";

struct FixtureContext {
    cyan::ast::Package package;
    cyan::TypeContext types;
    cyan::SemanticAnalysis analysis;
    const cyan::SourceFile* source = nullptr;
};

auto diagnostics_to_string(const cyan::DiagnosticList& diagnostics)
    -> std::string {
    std::ostringstream stream;
    cyan::print_diagnostics(stream, diagnostics);
    return stream.str();
}

auto write_fixture_file(std::string_view filename, std::string_view text)
    -> std::filesystem::path {
    auto path = std::filesystem::temp_directory_path() / filename;
    std::ofstream stream(path, std::ios::binary);
    stream << text;
    return path;
}

auto load_fixture_from_source(std::string_view filename,
                              std::string_view module_name,
                              std::string_view source_text)
    -> std::optional<FixtureContext> {
    FixtureContext context;

    const auto path = write_fixture_file(filename, source_text);
    auto source = cyan::SourceFile::load(path);
    std::filesystem::remove(path);
    if (!source) {
        std::cerr << "failed to load fixture: " << source.error() << '\n';
        return std::nullopt;
    }

    context.package.sources.push_back(
        std::make_unique<cyan::SourceFile>(std::move(*source)));
    context.source = context.package.sources.back().get();

    cyan::Lexer lexer(*context.source);
    auto tokens = lexer.lexAll();
    if (!tokens) {
        std::cerr << diagnostics_to_string(
            cyan::DiagnosticList{tokens.error()});
        return std::nullopt;
    }

    cyan::Parser parser(*context.source, std::move(*tokens));
    auto module = parser.parseModule();
    if (!module.diagnostics.empty()) {
        std::cerr << diagnostics_to_string(module.diagnostics);
        return std::nullopt;
    }

    module.source = context.source;
    module.path = context.source->path();
    module.module_name = std::string(module_name);
    context.package.modules.push_back(
        std::make_unique<cyan::ast::Module>(std::move(module)));
    context.package.entry_module = context.package.modules.back().get();

    cyan::SemanticAnalyzer sema(context.types);
    context.analysis = sema.analyze(context.package);
    if (!context.analysis.diagnostics.empty()) {
        std::cerr << diagnostics_to_string(context.analysis.diagnostics);
        return std::nullopt;
    }

    return context;
}

auto load_fixture() -> std::optional<FixtureContext> {
    return load_fixture_from_source("cyan_lsp_support.cyan",
                                    "lsp_support_fixture", FIXTURE_SOURCE);
}

auto nth_offset(std::string_view text, std::string_view needle, std::size_t nth)
    -> std::optional<std::size_t> {
    std::size_t offset = 0;
    for (std::size_t index = 0; index <= nth; ++index) {
        offset = text.find(needle, offset);
        if (offset == std::string_view::npos) {
            return std::nullopt;
        }
        if (index == nth) {
            return offset;
        }
        offset += needle.size();
    }
    return std::nullopt;
}

auto expect(bool condition, std::string message,
            std::vector<std::string>& failures) -> void {
    if (!condition) {
        failures.push_back(std::move(message));
    }
}

template <typename T>
auto parse_json(std::string_view json) -> std::optional<T> {
    T value{};
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(value, json)) {
        return std::nullopt;
    }
    return value;
}

template <typename T> auto to_json(const T& value) -> std::string {
    auto json = glz::write_json(value);
    return json.value_or("");
}

auto same_range(cyan::SourceRange lhs, cyan::SourceRange rhs) -> bool {
    return lhs.begin == rhs.begin && lhs.end == rhs.end &&
           lhs.source == rhs.source;
}

auto same_optional_range(const std::optional<cyan::SourceRange>& lhs,
                         cyan::SourceRange rhs) -> bool {
    return lhs.has_value() && same_range(*lhs, rhs);
}

auto find_occurrence(const std::vector<cyan::LSPSymbolOccurrence>& occurrences,
                     cyan::LSPSymbolKind kind, cyan::LSPSymbolRole role,
                     std::string_view name, std::size_t nth = 0)
    -> const cyan::LSPSymbolOccurrence* {
    std::size_t seen = 0;
    for (const auto& occurrence : occurrences) {
        if (occurrence.kind == kind && occurrence.role == role &&
            occurrence.name == name) {
            if (seen == nth) {
                return &occurrence;
            }
            ++seen;
        }
    }
    return nullptr;
}

auto offset_to_lsp_position(std::string_view text, std::size_t offset)
    -> lsp_test::Position {
    std::size_t line = 0;
    std::size_t character = 0;
    for (std::size_t index = 0; index < offset && index < text.size();
         ++index) {
        if (text[index] == '\n') {
            ++line;
            character = 0;
        } else {
            ++character;
        }
    }
    return {.line = line, .character = character};
}

auto frame_message(std::string_view body) -> std::string {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
           std::string(body);
}

auto read_framed_bodies(std::string_view stream) -> std::vector<std::string> {
    std::vector<std::string> bodies;
    std::size_t offset = 0;
    while (offset < stream.size()) {
        const auto header_end = stream.find("\r\n\r\n", offset);
        if (header_end == std::string_view::npos) {
            break;
        }
        const auto header = stream.substr(offset, header_end - offset);
        const auto prefix = header.find("Content-Length: ");
        if (prefix == std::string_view::npos) {
            break;
        }
        const auto value_begin =
            prefix + std::string_view("Content-Length: ").size();
        const auto value_end = header.find("\r\n", value_begin);
        const auto length_text =
            header.substr(value_begin, value_end - value_begin);
        const auto length =
            static_cast<std::size_t>(std::stoul(std::string(length_text)));
        const auto body_begin = header_end + 4;
        if (body_begin + length > stream.size()) {
            break;
        }
        bodies.emplace_back(stream.substr(body_begin, length));
        offset = body_begin + length;
    }
    return bodies;
}

auto find_message_by_id(const std::vector<lsp_test::MessageEnvelope>& messages,
                        std::string_view id)
    -> const lsp_test::MessageEnvelope* {
    for (const auto& message : messages) {
        if (message.id.has_value() && message.id->str == id) {
            return &message;
        }
    }
    return nullptr;
}

auto find_publish_diagnostics(
    const std::vector<lsp_test::MessageEnvelope>& messages, int version)
    -> const lsp_test::MessageEnvelope* {
    for (const auto& message : messages) {
        if (message.method != "textDocument/publishDiagnostics" ||
            !message.params.has_value()) {
            continue;
        }
        const auto params =
            parse_json<lsp_test::PublishDiagnosticsParams>(message.params->str);
        if (params.has_value() && params->version == version) {
            return &message;
        }
    }
    return nullptr;
}

auto test_document_symbols(const cyan::LSPSupport& lsp,
                           const cyan::SourceFile& source,
                           std::vector<std::string>& failures) -> void {
    const auto occurrences = lsp.documentSymbols(source);

    const auto* pair_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Struct,
                        cyan::LSPSymbolRole::Declaration, "Pair");
    expect(pair_decl != nullptr, "missing Pair declaration symbol", failures);

    const auto* pair_ref =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Struct,
                        cyan::LSPSymbolRole::Reference, "Pair");
    expect(pair_ref != nullptr, "missing Pair type reference symbol", failures);
    if (pair_decl != nullptr && pair_ref != nullptr) {
        expect(
            same_optional_range(pair_ref->declaration_range, pair_decl->range),
            "Pair reference should point at Pair declaration", failures);
    }

    const auto* some_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Variant,
                        cyan::LSPSymbolRole::Declaration, "Some");
    const auto* some_ref =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Variant,
                        cyan::LSPSymbolRole::Reference, "Some");
    expect(some_decl != nullptr, "missing Some declaration symbol", failures);
    expect(some_ref != nullptr, "missing Some reference symbol", failures);
    if (some_decl != nullptr && some_ref != nullptr) {
        expect(
            same_optional_range(some_ref->declaration_range, some_decl->range),
            "Some reference should point at Some declaration", failures);
    }

    const auto* payload_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::SwitchBinding,
                        cyan::LSPSymbolRole::Declaration, "payload");
    const auto* payload_ref =
        find_occurrence(occurrences, cyan::LSPSymbolKind::SwitchBinding,
                        cyan::LSPSymbolRole::Reference, "payload");
    expect(payload_decl != nullptr, "missing payload declaration symbol",
           failures);
    expect(payload_ref != nullptr, "missing payload reference symbol",
           failures);
    if (payload_decl != nullptr && payload_ref != nullptr) {
        expect(same_optional_range(payload_ref->declaration_range,
                                   payload_decl->range),
               "payload reference should point at switch binding declaration",
               failures);
    }

    const auto* field_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Field,
                        cyan::LSPSymbolRole::Declaration, "value");
    const auto* field_ref =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Field,
                        cyan::LSPSymbolRole::Reference, "value");
    expect(field_decl != nullptr, "missing field declaration symbol", failures);
    expect(field_ref != nullptr, "missing field reference symbol", failures);
    if (field_decl != nullptr && field_ref != nullptr) {
        expect(same_optional_range(field_ref->declaration_range,
                                   field_decl->range),
               "field reference should point at field declaration", failures);
    }
}

auto test_query(const cyan::LSPSupport& lsp, const cyan::SourceFile& source,
                std::vector<std::string>& failures) -> void {
    const auto text = source.text();

    const auto payload_ref_offset = nth_offset(text, "payload", 1);
    expect(payload_ref_offset.has_value(),
           "failed to find payload reference offset in fixture", failures);
    if (!payload_ref_offset.has_value()) {
        return;
    }

    const auto payload_result = lsp.query(source, *payload_ref_offset);
    expect(payload_result.has_value(),
           "query should resolve payload reference offset", failures);
    if (!payload_result.has_value()) {
        return;
    }

    expect(payload_result->enclosing_function != nullptr,
           "payload query should report enclosing function", failures);
    expect(payload_result->statement != nullptr,
           "payload query should report enclosing statement", failures);
    expect(payload_result->expression != nullptr,
           "payload query should report enclosing expression", failures);
    expect(payload_result->symbol.has_value(),
           "payload query should resolve a symbol occurrence", failures);
    if (payload_result->symbol.has_value()) {
        expect(payload_result->symbol->kind ==
                   cyan::LSPSymbolKind::SwitchBinding,
               "payload query should resolve switch binding symbol", failures);
        expect(payload_result->symbol->role == cyan::LSPSymbolRole::Reference,
               "payload query should resolve reference role", failures);
    }

    const auto pair_ref_offset = nth_offset(text, "Pair", 1);
    expect(pair_ref_offset.has_value(),
           "failed to find Pair type reference offset in fixture", failures);
    if (pair_ref_offset.has_value()) {
        const auto pair_result = lsp.query(source, *pair_ref_offset);
        expect(pair_result.has_value(),
               "query should resolve Pair type reference offset", failures);
        if (pair_result.has_value()) {
            expect(pair_result->type_syntax != nullptr,
                   "Pair type query should report enclosing type syntax",
                   failures);
            expect(pair_result->symbol.has_value(),
                   "Pair type query should resolve a symbol occurrence",
                   failures);
            if (pair_result->symbol.has_value()) {
                expect(pair_result->symbol->kind == cyan::LSPSymbolKind::Struct,
                       "Pair type query should resolve struct symbol",
                       failures);
            }
        }
    }

    const auto some_ref_offset = nth_offset(text, "Some", 1);
    expect(some_ref_offset.has_value(),
           "failed to find Some case reference offset in fixture", failures);
    if (some_ref_offset.has_value()) {
        const auto some_result = lsp.query(source, *some_ref_offset);
        expect(some_result.has_value(),
               "query should resolve Some case reference offset", failures);
        if (some_result.has_value()) {
            expect(some_result->symbol.has_value(),
                   "Some query should resolve a symbol occurrence", failures);
            if (some_result->symbol.has_value()) {
                expect(some_result->symbol->kind ==
                           cyan::LSPSymbolKind::Variant,
                       "Some query should resolve variant symbol", failures);
            }
        }
    }

    const auto location = source.location(*payload_ref_offset);
    const auto round_trip_offset = lsp.offsetForLocation(source, location);
    expect(round_trip_offset == payload_ref_offset,
           "offsetForLocation should round-trip byte locations", failures);
}

auto test_depends_shorthand_document_symbols(std::vector<std::string>& failures)
    -> void {
    auto context = load_fixture_from_source("cyan_lsp_depends_fixture.cyan",
                                            "lsp_depends_fixture",
                                            DEPENDS_SHORTHAND_FIXTURE_SOURCE);
    expect(context.has_value(),
           "failed to load shorthand depends fixture for lsp test", failures);
    if (!context.has_value()) {
        return;
    }

    cyan::LSPSupport lsp(context->package, context->analysis);
    const auto occurrences = lsp.documentSymbols(*context->source);

    const auto depends_text_offset =
        nth_offset(DEPENDS_SHORTHAND_FIXTURE_SOURCE, "text", 1);
    expect(depends_text_offset.has_value(),
           "failed to find depends source text offset in shorthand fixture",
           failures);
    if (!depends_text_offset.has_value()) {
        return;
    }

    const auto depends_text_range = context->source->range(
        *depends_text_offset,
        *depends_text_offset + std::string_view("text").size());
    std::size_t depends_reference_count = 0;
    const cyan::LSPSymbolOccurrence* depends_reference = nullptr;
    for (const auto& occurrence : occurrences) {
        if (occurrence.kind != cyan::LSPSymbolKind::Parameter ||
            occurrence.role != cyan::LSPSymbolRole::Reference ||
            occurrence.name != "text" ||
            !same_range(occurrence.range, depends_text_range)) {
            continue;
        }
        ++depends_reference_count;
        if (depends_reference == nullptr) {
            depends_reference = &occurrence;
        }
    }

    expect(depends_reference_count == 1,
           "depends clause should emit exactly one parameter reference "
           "occurrence",
           failures);
    expect(depends_reference != nullptr,
           "missing parameter reference occurrence for depends source",
           failures);

    const auto* parameter_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Parameter,
                        cyan::LSPSymbolRole::Declaration, "text");
    expect(parameter_decl != nullptr,
           "missing parameter declaration for shorthand depends fixture",
           failures);
    if (depends_reference != nullptr && parameter_decl != nullptr) {
        expect(same_optional_range(depends_reference->declaration_range,
                                   parameter_decl->range),
               "depends source reference should resolve to parameter "
               "declaration",
               failures);
    }
}

auto test_interface_alias_document_symbols(std::vector<std::string>& failures)
    -> void {
    auto context = load_fixture_from_source(
        "cyan_lsp_interface_alias_fixture.cyan", "lsp_interface_alias_fixture",
        INTERFACE_ALIAS_FIXTURE_SOURCE);
    expect(context.has_value(),
           "failed to load interface alias fixture for lsp test", failures);
    if (!context.has_value()) {
        return;
    }

    cyan::LSPSupport lsp(context->package, context->analysis);
    const auto occurrences = lsp.documentSymbols(*context->source);

    const auto* left_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Interface,
                        cyan::LSPSymbolRole::Declaration, "left");
    const auto* right_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Interface,
                        cyan::LSPSymbolRole::Declaration, "right");
    const auto* second_decl =
        find_occurrence(occurrences, cyan::LSPSymbolKind::Interface,
                        cyan::LSPSymbolRole::Declaration, "second");
    expect(left_decl != nullptr, "missing left interface declaration symbol",
           failures);
    expect(right_decl != nullptr, "missing right interface declaration symbol",
           failures);
    expect(second_decl != nullptr, "missing second alias declaration symbol",
           failures);
    if (left_decl == nullptr || right_decl == nullptr ||
        second_decl == nullptr) {
        return;
    }

    const auto alias_terms_offset =
        INTERFACE_ALIAS_FIXTURE_SOURCE.find("left + right");
    expect(alias_terms_offset != std::string_view::npos,
           "failed to find alias term source range", failures);
    if (alias_terms_offset == std::string_view::npos) {
        return;
    }

    const auto left_term_range = context->source->range(
        alias_terms_offset,
        alias_terms_offset + std::string_view("left").size());
    const auto right_term_begin =
        alias_terms_offset + std::string_view("left + ").size();
    const auto right_term_range = context->source->range(
        right_term_begin, right_term_begin + std::string_view("right").size());

    const auto find_reference_at_range =
        [&](std::string_view name,
            cyan::SourceRange range) -> const cyan::LSPSymbolOccurrence* {
        const auto it = std::ranges::find_if(
            occurrences, [&](const cyan::LSPSymbolOccurrence& occ) {
                return occ.kind == cyan::LSPSymbolKind::Interface &&
                       occ.role == cyan::LSPSymbolRole::Reference &&
                       occ.name == name && same_range(occ.range, range);
            });
        return it == occurrences.end() ? nullptr : &*it;
    };

    const auto* left_term_ref =
        find_reference_at_range("left", left_term_range);
    const auto* right_term_ref =
        find_reference_at_range("right", right_term_range);
    expect(left_term_ref != nullptr,
           "interface alias term should emit left reference occurrence",
           failures);
    expect(right_term_ref != nullptr,
           "interface alias term should emit right reference occurrence",
           failures);
    if (left_term_ref != nullptr) {
        expect(same_optional_range(left_term_ref->declaration_range,
                                   left_decl->range),
               "left alias term should resolve to left interface declaration",
               failures);
    }
    if (right_term_ref != nullptr) {
        expect(same_optional_range(right_term_ref->declaration_range,
                                   right_decl->range),
               "right alias term should resolve to right interface declaration",
               failures);
    }

    const auto second_type_offset =
        INTERFACE_ALIAS_FIXTURE_SOURCE.find("&second value");
    expect(second_type_offset != std::string_view::npos,
           "failed to find second alias type usage", failures);
    if (second_type_offset != std::string_view::npos) {
        const auto second_type_range = context->source->range(
            second_type_offset + 1,
            second_type_offset + 1 + std::string_view("second").size());
        const auto* second_type_ref =
            find_reference_at_range("second", second_type_range);
        expect(second_type_ref != nullptr,
               "alias type usage should emit second reference occurrence",
               failures);
        if (second_type_ref != nullptr) {
            expect(same_optional_range(second_type_ref->declaration_range,
                                       second_decl->range),
                   "second alias type usage should resolve to second alias "
                   "declaration",
                   failures);
        }
    }
}

auto test_language_server(std::vector<std::string>& failures) -> void {
    const auto temp_path =
        std::filesystem::temp_directory_path() / "cyan_lsp_server_fixture.cyan";
    const auto uri = "file://" + temp_path.generic_string();

    const auto payload_ref_offset = nth_offset(FIXTURE_SOURCE, "payload", 1);
    const auto payload_decl_offset = nth_offset(FIXTURE_SOURCE, "payload", 0);
    expect(payload_ref_offset.has_value(),
           "failed to find payload offset for lsp server test", failures);
    expect(payload_decl_offset.has_value(),
           "failed to find payload declaration offset for lsp server test",
           failures);
    if (!payload_ref_offset.has_value()) {
        return;
    }
    const auto hover_position =
        offset_to_lsp_position(FIXTURE_SOURCE, *payload_ref_offset);
    const auto definition_position =
        offset_to_lsp_position(FIXTURE_SOURCE, *payload_ref_offset);
    const auto payload_decl_position =
        payload_decl_offset.has_value()
            ? std::optional<lsp_test::Position>(
                  offset_to_lsp_position(FIXTURE_SOURCE, *payload_decl_offset))
            : std::nullopt;

    const std::string initialize_request =
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
    const std::string did_open_notification =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri + R"(","languageId":"cyan","version":1,"text":)" +
        to_json(std::string(FIXTURE_SOURCE)) + "}}}";
    const std::string hover_request =
        R"({"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":")" +
        uri + R"("},"position":{"line":)" +
        std::to_string(hover_position.line) + R"(,"character":)" +
        std::to_string(hover_position.character) + "}}}";
    const std::string definition_request =
        R"({"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":")" +
        uri + R"("},"position":{"line":)" +
        std::to_string(definition_position.line) + R"(,"character":)" +
        std::to_string(definition_position.character) + "}}}";
    const std::string semantic_tokens_request =
        R"({"jsonrpc":"2.0","id":4,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":")" +
        uri + R"("}}})";
    const std::string did_change_notification =
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")" +
        uri + R"(","version":2},"contentChanges":[{"text":"i64 broken( {"}]}})";
    const std::string shutdown_request =
        R"({"jsonrpc":"2.0","id":5,"method":"shutdown","params":{}})";
    const std::string exit_notification =
        R"({"jsonrpc":"2.0","method":"exit"})";

    std::string input_stream;
    input_stream += frame_message(initialize_request);
    input_stream += frame_message(did_open_notification);
    input_stream += frame_message(hover_request);
    input_stream += frame_message(definition_request);
    input_stream += frame_message(semantic_tokens_request);
    input_stream += frame_message(did_change_notification);
    input_stream += frame_message(shutdown_request);
    input_stream += frame_message(exit_notification);

    cyan::LanguageServer server;
    std::istringstream input(input_stream);
    std::ostringstream output;
    const auto exit_code = server.run(input, output);
    expect(exit_code == 0, "language server should exit cleanly after shutdown",
           failures);

    const auto bodies = read_framed_bodies(output.str());
    std::vector<lsp_test::MessageEnvelope> messages;
    for (const auto& body : bodies) {
        const auto parsed = parse_json<lsp_test::MessageEnvelope>(body);
        expect(parsed.has_value(), "failed to parse language server response",
               failures);
        if (parsed.has_value()) {
            messages.push_back(*parsed);
        }
    }

    std::vector<std::string> semantic_token_types;
    const auto* initialize_response = find_message_by_id(messages, "1");
    expect(initialize_response != nullptr,
           "missing initialize response from language server", failures);
    if (initialize_response != nullptr &&
        initialize_response->result.has_value()) {
        const auto result = parse_json<lsp_test::InitializeResult>(
            initialize_response->result->str);
        expect(result.has_value(), "failed to parse initialize result",
               failures);
        if (result.has_value()) {
            expect(result->capabilities.hoverProvider,
                   "initialize should advertise hover support", failures);
            expect(result->capabilities.definitionProvider,
                   "initialize should advertise definition support", failures);
            expect(result->capabilities.renameProvider,
                   "initialize should advertise rename support", failures);
            expect(result->capabilities.textDocumentSync.openClose,
                   "initialize should advertise openClose sync", failures);
            expect(result->capabilities.textDocumentSync.change == 1,
                   "initialize should advertise full text sync", failures);
            expect(!result->capabilities.semanticTokensProvider.legend
                        .tokenTypes.empty(),
                   "initialize should advertise semantic token types",
                   failures);
            semantic_token_types =
                result->capabilities.semanticTokensProvider.legend.tokenTypes;
        }
    }

    const auto* initial_diagnostics = find_publish_diagnostics(messages, 1);
    expect(initial_diagnostics != nullptr,
           "missing initial publishDiagnostics notification", failures);
    if (initial_diagnostics != nullptr &&
        initial_diagnostics->params.has_value()) {
        const auto params = parse_json<lsp_test::PublishDiagnosticsParams>(
            initial_diagnostics->params->str);
        expect(params.has_value(),
               "failed to parse initial diagnostics notification", failures);
        if (params.has_value()) {
            expect(params->diagnostics.empty(),
                   "initial diagnostics should be empty for valid source",
                   failures);
        }
    }

    const auto* hover_response = find_message_by_id(messages, "2");
    expect(hover_response != nullptr, "missing hover response", failures);
    if (hover_response != nullptr && hover_response->result.has_value()) {
        const auto hover =
            parse_json<lsp_test::Hover>(hover_response->result->str);
        expect(hover.has_value(), "failed to parse hover response", failures);
        if (hover.has_value()) {
            expect(hover->contents.value.find("payload: i64") !=
                       std::string::npos,
                   "hover should include the payload type", failures);
        }
    }

    const auto* definition_response = find_message_by_id(messages, "3");
    expect(definition_response != nullptr, "missing definition response",
           failures);
    if (definition_response != nullptr &&
        definition_response->result.has_value()) {
        const auto definition =
            parse_json<lsp_test::Location>(definition_response->result->str);
        expect(definition.has_value(), "failed to parse definition response",
               failures);
        if (definition.has_value()) {
            expect(definition->uri == uri,
                   "definition should point at the opened document", failures);
            if (payload_decl_position.has_value()) {
                expect(definition->range.start.line ==
                               payload_decl_position->line &&
                           definition->range.start.character ==
                               payload_decl_position->character,
                       "definition should jump to payload declaration",
                       failures);
            }
        }
    }

    const auto* semantic_tokens_response = find_message_by_id(messages, "4");
    expect(semantic_tokens_response != nullptr,
           "missing semantic tokens response", failures);
    if (semantic_tokens_response != nullptr &&
        semantic_tokens_response->result.has_value()) {
        const auto tokens = parse_json<lsp_test::SemanticTokens>(
            semantic_tokens_response->result->str);
        expect(tokens.has_value(), "failed to parse semantic tokens response",
               failures);
        if (tokens.has_value()) {
            expect(!tokens->data.empty(),
                   "semantic tokens response should not be empty", failures);
            expect(tokens->data.size() % 5 == 0,
                   "semantic token payload should be encoded in 5-tuples",
                   failures);
            const auto contains_token_type =
                [&](std::string_view token_type_name) {
                    const auto it = std::ranges::find(semantic_token_types,
                                                      token_type_name);
                    expect(it != semantic_token_types.end(),
                           "semantic token legend should include " +
                               std::string(token_type_name),
                           failures);
                    if (it == semantic_token_types.end()) {
                        return;
                    }
                    const auto type_index = static_cast<std::uint32_t>(
                        std::distance(semantic_token_types.begin(), it));
                    bool found = false;
                    for (std::size_t index = 3; index < tokens->data.size();
                         index += 5) {
                        if (tokens->data[index] == type_index) {
                            found = true;
                            break;
                        }
                    }
                    expect(found,
                           "semantic tokens should include " +
                               std::string(token_type_name) + " entries",
                           failures);
                };
            contains_token_type("keyword");
            contains_token_type("number");
            contains_token_type("operator");
        }
    }

    const auto* changed_diagnostics = find_publish_diagnostics(messages, 2);
    expect(changed_diagnostics != nullptr,
           "missing diagnostics after change notification", failures);
    if (changed_diagnostics != nullptr &&
        changed_diagnostics->params.has_value()) {
        const auto params = parse_json<lsp_test::PublishDiagnosticsParams>(
            changed_diagnostics->params->str);
        expect(params.has_value(),
               "failed to parse changed diagnostics notification", failures);
        if (params.has_value()) {
            expect(!params->diagnostics.empty(),
                   "changed source should produce diagnostics", failures);
        }
    }

    const auto* shutdown_response = find_message_by_id(messages, "5");
    expect(shutdown_response != nullptr, "missing shutdown response", failures);
}

auto test_language_server_impl_definition_and_rename(
    std::vector<std::string>& failures) -> void {
    const auto path = write_fixture_file("cyan_lsp_interface_fixture.cyan",
                                         INTERFACE_FIXTURE_SOURCE);
    const auto uri = "file://" + path.generic_string();

    const auto interface_decl_offset =
        nth_offset(INTERFACE_FIXTURE_SOURCE, "measure", 0);
    const auto impl_decl_offset =
        nth_offset(INTERFACE_FIXTURE_SOURCE, "measure", 1);
    const auto call_offset = nth_offset(INTERFACE_FIXTURE_SOURCE, "measure", 2);
    expect(interface_decl_offset.has_value(),
           "failed to find interface declaration offset", failures);
    expect(impl_decl_offset.has_value(),
           "failed to find impl declaration offset", failures);
    expect(call_offset.has_value(), "failed to find call offset", failures);
    if (!interface_decl_offset.has_value() || !impl_decl_offset.has_value() ||
        !call_offset.has_value()) {
        std::filesystem::remove(path);
        return;
    }

    const auto interface_position = offset_to_lsp_position(
        INTERFACE_FIXTURE_SOURCE, *interface_decl_offset);
    const auto impl_end_position = offset_to_lsp_position(
        INTERFACE_FIXTURE_SOURCE,
        *impl_decl_offset + std::string_view("measure").size());
    const auto call_position =
        offset_to_lsp_position(INTERFACE_FIXTURE_SOURCE, *call_offset);

    const std::string initialize_request =
        R"({"jsonrpc":"2.0","id":11,"method":"initialize","params":{}})";
    const std::string did_open_notification =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri + R"(","languageId":"cyan","version":1,"text":)" +
        to_json(std::string(INTERFACE_FIXTURE_SOURCE)) + "}}}";
    const std::string definition_request =
        R"({"jsonrpc":"2.0","id":12,"method":"textDocument/definition","params":{"textDocument":{"uri":")" +
        uri + R"("},"position":{"line":)" +
        std::to_string(impl_end_position.line) + R"(,"character":)" +
        std::to_string(impl_end_position.character) + "}}}";
    const std::string rename_request =
        R"({"jsonrpc":"2.0","id":13,"method":"textDocument/rename","params":{"textDocument":{"uri":")" +
        uri + R"("},"position":{"line":)" +
        std::to_string(impl_end_position.line) + R"(,"character":)" +
        std::to_string(impl_end_position.character) +
        R"(},"newName":"inspect"}})";
    const std::string shutdown_request =
        R"({"jsonrpc":"2.0","id":14,"method":"shutdown","params":{}})";
    const std::string exit_notification =
        R"({"jsonrpc":"2.0","method":"exit"})";

    std::string input_stream;
    input_stream += frame_message(initialize_request);
    input_stream += frame_message(did_open_notification);
    input_stream += frame_message(definition_request);
    input_stream += frame_message(rename_request);
    input_stream += frame_message(shutdown_request);
    input_stream += frame_message(exit_notification);

    cyan::LanguageServer server;
    std::istringstream input(input_stream);
    std::ostringstream output;
    const auto exit_code = server.run(input, output);
    expect(exit_code == 0, "impl definition/rename server should exit cleanly",
           failures);

    const auto bodies = read_framed_bodies(output.str());
    std::vector<lsp_test::MessageEnvelope> messages;
    for (const auto& body : bodies) {
        const auto parsed = parse_json<lsp_test::MessageEnvelope>(body);
        expect(parsed.has_value(),
               "failed to parse impl definition/rename response", failures);
        if (parsed.has_value()) {
            messages.push_back(*parsed);
        }
    }

    const auto* definition_response = find_message_by_id(messages, "12");
    expect(definition_response != nullptr, "missing impl definition response",
           failures);
    if (definition_response != nullptr &&
        definition_response->result.has_value()) {
        const auto definition =
            parse_json<lsp_test::Location>(definition_response->result->str);
        expect(definition.has_value(),
               "failed to parse impl definition response", failures);
        if (definition.has_value()) {
            expect(definition->uri == uri,
                   "impl definition should remain in the same file", failures);
            expect(definition->range.start.line == interface_position.line &&
                       definition->range.start.character ==
                           interface_position.character,
                   "impl definition should jump to interface declaration",
                   failures);
        }
    }

    const auto* rename_response = find_message_by_id(messages, "13");
    expect(rename_response != nullptr, "missing rename response", failures);
    if (rename_response != nullptr && rename_response->result.has_value()) {
        const auto edit =
            parse_json<lsp_test::WorkspaceEdit>(rename_response->result->str);
        expect(edit.has_value(), "failed to parse rename workspace edit",
               failures);
        if (edit.has_value()) {
            const auto it = edit->changes.find(uri);
            expect(it != edit->changes.end(),
                   "rename should include edits for the opened file", failures);
            if (it != edit->changes.end()) {
                const auto& edits = it->second;
                expect(edits.size() == 3,
                       "rename should update interface, impl, and call",
                       failures);
                bool saw_interface = false;
                bool saw_impl = false;
                bool saw_call = false;
                for (const auto& text_edit : edits) {
                    expect(text_edit.newText == "inspect",
                           "rename should write the requested identifier",
                           failures);
                    if (text_edit.range.start.line == interface_position.line &&
                        text_edit.range.start.character ==
                            interface_position.character) {
                        saw_interface = true;
                    }
                    if (text_edit.range.start.line == impl_end_position.line &&
                        text_edit.range.start.character +
                                std::string_view("measure").size() ==
                            impl_end_position.character) {
                        saw_impl = true;
                    }
                    if (text_edit.range.start.line == call_position.line &&
                        text_edit.range.start.character ==
                            call_position.character) {
                        saw_call = true;
                    }
                }
                expect(saw_interface,
                       "rename should include the interface declaration",
                       failures);
                expect(saw_impl, "rename should include the impl declaration",
                       failures);
                expect(saw_call, "rename should include the call site",
                       failures);
            }
        }
    }

    const auto* shutdown_response = find_message_by_id(messages, "14");
    expect(shutdown_response != nullptr,
           "missing impl definition/rename shutdown response", failures);

    std::filesystem::remove(path);
}

auto test_language_server_builtin_definition(std::vector<std::string>& failures)
    -> void {
    const auto path = write_fixture_file("cyan_lsp_builtin_fixture.cyan",
                                         BUILTIN_IMPORT_FIXTURE_SOURCE);
    const auto uri = "file://" + path.generic_string();

    const auto call_offset =
        nth_offset(BUILTIN_IMPORT_FIXTURE_SOURCE, "memcpy", 0);
    expect(call_offset.has_value(),
           "failed to find builtin definition call offset", failures);
    if (!call_offset.has_value()) {
        std::filesystem::remove(path);
        return;
    }
    const auto call_position =
        offset_to_lsp_position(BUILTIN_IMPORT_FIXTURE_SOURCE, *call_offset);

    const auto builtin_path =
        std::filesystem::absolute(std::filesystem::path(__FILE__))
            .parent_path()
            .parent_path() /
        "stdlib" / "std" / "mem.cyan";
    std::ifstream builtin_stream(builtin_path, std::ios::binary);
    std::ostringstream builtin_buffer;
    builtin_buffer << builtin_stream.rdbuf();
    const auto builtin_source = builtin_buffer.str();
    const auto builtin_decl_offset = nth_offset(builtin_source, "memcpy", 1);
    expect(builtin_decl_offset.has_value(),
           "failed to find builtin memcpy declaration offset", failures);
    if (!builtin_decl_offset.has_value()) {
        std::filesystem::remove(path);
        return;
    }
    const auto builtin_decl_position =
        offset_to_lsp_position(builtin_source, *builtin_decl_offset);
    const std::string builtin_uri = "cyan-stdlib:/std/mem.cyan";

    const std::string initialize_request =
        R"({"jsonrpc":"2.0","id":21,"method":"initialize","params":{}})";
    const std::string did_open_notification =
        R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")" +
        uri + R"(","languageId":"cyan","version":1,"text":)" +
        to_json(std::string(BUILTIN_IMPORT_FIXTURE_SOURCE)) + "}}}";
    const std::string definition_request =
        R"({"jsonrpc":"2.0","id":22,"method":"textDocument/definition","params":{"textDocument":{"uri":")" +
        uri + R"("},"position":{"line":)" + std::to_string(call_position.line) +
        R"(,"character":)" + std::to_string(call_position.character) + "}}}";
    const std::string builtin_source_request =
        R"({"jsonrpc":"2.0","id":24,"method":"cyan/builtinSource","params":{"uri":")" +
        builtin_uri + R"("}})";
    const std::string shutdown_request =
        R"({"jsonrpc":"2.0","id":23,"method":"shutdown","params":{}})";
    const std::string exit_notification =
        R"({"jsonrpc":"2.0","method":"exit"})";

    std::string input_stream;
    input_stream += frame_message(initialize_request);
    input_stream += frame_message(did_open_notification);
    input_stream += frame_message(definition_request);
    input_stream += frame_message(builtin_source_request);
    input_stream += frame_message(shutdown_request);
    input_stream += frame_message(exit_notification);

    cyan::LanguageServer server;
    std::istringstream input(input_stream);
    std::ostringstream output;
    const auto exit_code = server.run(input, output);
    expect(exit_code == 0,
           "builtin definition server should exit cleanly after shutdown",
           failures);

    const auto bodies = read_framed_bodies(output.str());
    std::vector<lsp_test::MessageEnvelope> messages;
    for (const auto& body : bodies) {
        const auto parsed = parse_json<lsp_test::MessageEnvelope>(body);
        expect(parsed.has_value(),
               "failed to parse builtin definition response", failures);
        if (parsed.has_value()) {
            messages.push_back(*parsed);
        }
    }

    const auto* builtin_source_response = find_message_by_id(messages, "24");
    expect(builtin_source_response != nullptr,
           "missing builtin source response", failures);
    if (builtin_source_response != nullptr &&
        builtin_source_response->result.has_value()) {
        const auto builtin_result = parse_json<lsp_test::BuiltinSourceResult>(
            builtin_source_response->result->str);
        expect(builtin_result.has_value(),
               "failed to parse builtin source response", failures);
        if (builtin_result.has_value()) {
            expect(builtin_result->text == builtin_source,
                   "builtin source response should return the embedded stdlib "
                   "text",
                   failures);
        }
    }

    const auto* definition_response = find_message_by_id(messages, "22");
    expect(definition_response != nullptr,
           "missing builtin definition response", failures);
    if (definition_response != nullptr &&
        definition_response->result.has_value()) {
        const auto definition =
            parse_json<lsp_test::Location>(definition_response->result->str);
        expect(definition.has_value(),
               "failed to parse builtin definition response", failures);
        if (definition.has_value()) {
            expect(definition->uri == builtin_uri,
                   "builtin definition should jump into stdlib source",
                   failures);
            expect(
                definition->range.start.line == builtin_decl_position.line,
                "builtin definition should land on the memcpy declaration line",
                failures);
        }
    }

    std::filesystem::remove(path);
}

} // namespace

auto main() -> int {
    auto context = load_fixture();
    if (!context.has_value()) {
        return 1;
    }

    cyan::LSPSupport lsp(context->package, context->analysis);
    std::vector<std::string> failures;

    test_document_symbols(lsp, *context->source, failures);
    test_query(lsp, *context->source, failures);
    test_depends_shorthand_document_symbols(failures);
    test_interface_alias_document_symbols(failures);
    test_language_server(failures);
    test_language_server_impl_definition_and_rename(failures);
    test_language_server_builtin_definition(failures);

    if (!failures.empty()) {
        for (const auto& failure : failures) {
            std::cerr << failure << '\n';
        }
        return 1;
    }

    return 0;
}

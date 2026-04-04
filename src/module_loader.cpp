#include "cyan/module_loader.hpp"

#include "cyan/lexer.hpp"
#include "cyan/parser.hpp"
#include "cyan/source.hpp"

#include <algorithm>
#include <cctype>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cyan {

namespace {

constexpr std::string_view BUILTIN_ROOT_TEXT = "/__cyan_builtin__";

static constexpr char k_std_heap_source[] = {
#embed "../stdlib/std/heap.cyan"
    , 0};
static constexpr char k_std_mem_source[] = {
#embed "../stdlib/std/mem.cyan"
    , 0};
static constexpr char k_std_cstr_source[] = {
#embed "../stdlib/std/cstr.cyan"
    , 0};
static constexpr char k_std_file_source[] = {
#embed "../stdlib/std/file.cyan"
    , 0};
static constexpr char k_std_println_source[] = {
#embed "../stdlib/std/println.cyan"
    , 0};
static constexpr char k_std_strconv_source[] = {
#embed "../stdlib/std/strconv.cyan"
    , 0};
static constexpr char k_std_ryu_source[] = {
#embed "../stdlib/std/ryu.cyan"
    , 0};
static constexpr char k_std_ptr_source[] = {
#embed "../stdlib/std/ptr.cyan"
    , 0};
static constexpr char k_std_result_source[] = {
#embed "../stdlib/std/result.cyan"
    , 0};
static constexpr char k_std_err_source[] = {
#embed "../stdlib/std/err.cyan"
    , 0};
static constexpr char k_std_error_source[] = {
#embed "../stdlib/std/error.cyan"
    , 0};
static constexpr char k_std_fmt_source[] = {
#embed "../stdlib/std/fmt.cyan"
    , 0};
static constexpr char k_std_panic_source[] = {
#embed "../stdlib/std/panic.cyan"
    , 0};
static constexpr char k_std_abi_source[] = {
#embed "../stdlib/std/abi.cyan"
    , 0};
static constexpr char k_std_atomic_source[] = {
#embed "../stdlib/std/atomic.cyan"
    , 0};
static constexpr char k_std_rc_source[] = {
#embed "../stdlib/std/rc.cyan"
    , 0};
static constexpr char k_std_view_source[] = {
#embed "../stdlib/std/view.cyan"
    , 0};
static constexpr char k_std_sync_source[] = {
#embed "../stdlib/std/sync.cyan"
    , 0};
static constexpr char k_std_thread_source[] = {
#embed "../stdlib/std/thread.cyan"
    , 0};
static constexpr char k_std_math_source[] = {
#embed "../stdlib/std/math.cyan"
    , 0};
static constexpr char k_std_math_intrin_source[] = {
#embed "../stdlib/std/math/intrin.cyan"
    , 0};

struct PendingLowering {
    SourceRange range;
    std::size_t target_offset = 0;
    std::string intrinsic_name;
    ast::LoweringConstantKind constant_kind = ast::LoweringConstantKind::None;
    ast::BuiltinCallKind builtin_kind = ast::BuiltinCallKind::None;
    std::optional<std::string> intrinsic_return_spec;
    std::optional<std::size_t> return_argument_index;
    std::vector<ast::IntrinsicLoweringArgument> argument_overrides;
    std::vector<ast::ExternSymbolCase> intrinsic_cases;
    std::vector<ast::ExternSymbolCase> extern_symbol_cases;
};

auto builtin_root_path() -> const std::filesystem::path& {
    static const auto path = std::filesystem::path(BUILTIN_ROOT_TEXT);
    return path;
}

auto builtin_module_sources()
    -> const std::unordered_map<std::string_view, std::string_view>& {
    static const auto modules =
        std::unordered_map<std::string_view, std::string_view>{
            {"std.heap", std::string_view(k_std_heap_source,
                                          sizeof(k_std_heap_source) - 1)},
            {"std.mem",
             std::string_view(k_std_mem_source, sizeof(k_std_mem_source) - 1)},
            {"std.cstr", std::string_view(k_std_cstr_source,
                                          sizeof(k_std_cstr_source) - 1)},
            {"std.file", std::string_view(k_std_file_source,
                                          sizeof(k_std_file_source) - 1)},
            {"std.println", std::string_view(k_std_println_source,
                                             sizeof(k_std_println_source) - 1)},
            {"std.strconv", std::string_view(k_std_strconv_source,
                                             sizeof(k_std_strconv_source) - 1)},
            {"std.ryu",
             std::string_view(k_std_ryu_source, sizeof(k_std_ryu_source) - 1)},
            {"std.ptr",
             std::string_view(k_std_ptr_source, sizeof(k_std_ptr_source) - 1)},
            {"std.result", std::string_view(k_std_result_source,
                                            sizeof(k_std_result_source) - 1)},
            {"std.err",
             std::string_view(k_std_err_source, sizeof(k_std_err_source) - 1)},
            {"std.error", std::string_view(k_std_error_source,
                                           sizeof(k_std_error_source) - 1)},
            {"std.fmt",
             std::string_view(k_std_fmt_source, sizeof(k_std_fmt_source) - 1)},
            {"std.panic", std::string_view(k_std_panic_source,
                                           sizeof(k_std_panic_source) - 1)},
            {"std.abi",
             std::string_view(k_std_abi_source, sizeof(k_std_abi_source) - 1)},
            {"std.atomic", std::string_view(k_std_atomic_source,
                                            sizeof(k_std_atomic_source) - 1)},
            {"std.rc",
             std::string_view(k_std_rc_source, sizeof(k_std_rc_source) - 1)},
            {"std.view", std::string_view(k_std_view_source,
                                          sizeof(k_std_view_source) - 1)},
            {"std.sync", std::string_view(k_std_sync_source,
                                          sizeof(k_std_sync_source) - 1)},
            {"std.thread", std::string_view(k_std_thread_source,
                                            sizeof(k_std_thread_source) - 1)},
            {"std.math", std::string_view(k_std_math_source,
                                          sizeof(k_std_math_source) - 1)},
            {"std.math.intrin",
             std::string_view(k_std_math_intrin_source,
                              sizeof(k_std_math_intrin_source) - 1)},
        };
    return modules;
}

auto trim_ascii(std::string_view text) -> std::string_view {
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

auto skip_trivia(std::string_view text, std::size_t offset) -> std::size_t {
    while (offset < text.size()) {
        const auto ch = text[offset];
        if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
            ++offset;
            continue;
        }
        if (offset + 1 < text.size() && text[offset] == '/' &&
            text[offset + 1] == '/') {
            offset += 2;
            while (offset < text.size() && text[offset] != '\n') {
                ++offset;
            }
            continue;
        }
        break;
    }
    return offset;
}

auto starts_with_keyword(std::string_view text, std::size_t offset,
                         std::string_view keyword) -> bool {
    if (offset + keyword.size() > text.size() ||
        text.substr(offset, keyword.size()) != keyword) {
        return false;
    }
    const auto next = offset + keyword.size();
    return next >= text.size() ||
           (!std::isalnum(static_cast<unsigned char>(text[next])) &&
            text[next] != '_');
}

auto skip_decl_prefix(std::string_view text, std::size_t offset)
    -> std::size_t {
    offset = skip_trivia(text, offset);
    if (starts_with_keyword(text, offset, "export")) {
        offset = skip_trivia(text, offset + std::string_view("export").size());
    }
    while (true) {
        if (starts_with_keyword(text, offset, "const")) {
            offset =
                skip_trivia(text, offset + std::string_view("const").size());
            continue;
        }
        if (starts_with_keyword(text, offset, "shared")) {
            offset =
                skip_trivia(text, offset + std::string_view("shared").size());
            continue;
        }
        break;
    }
    return offset;
}

auto parse_argument_index(std::string_view text)
    -> std::expected<std::size_t, std::string> {
    if (!text.starts_with("arg") || text.size() <= 3) {
        return std::unexpected("expected 'argN'");
    }
    std::size_t value = 0;
    for (std::size_t index = 3; index < text.size(); ++index) {
        const auto ch = text[index];
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
            return std::unexpected("expected decimal argument index");
        }
        value = value * 10 +
                static_cast<std::size_t>(ch - static_cast<unsigned char>('0'));
    }
    return value;
}

auto parse_return_argument_index(std::string_view text)
    -> std::expected<std::optional<std::size_t>, std::string> {
    if (text == "void") {
        return std::nullopt;
    }
    auto value = parse_argument_index(text);
    if (!value) {
        return std::unexpected("expected 'void' or 'argN'");
    }
    return *value;
}

auto parse_builtin_lowering_kind(std::string_view text)
    -> std::optional<ast::BuiltinCallKind> {
    using enum ast::BuiltinCallKind;
    static const auto builtins =
        std::unordered_map<std::string_view, ast::BuiltinCallKind>{
            {"len", Len},
            {"subslice", Subslice},
            {"raw_data", RawData},
            {"fn_ptr", FunctionPointer},
            {"panic", Panic},
            {"atomic_relaxed", AtomicRelaxedOrder},
            {"atomic_acquire", AtomicAcquireOrder},
            {"atomic_release", AtomicReleaseOrder},
            {"atomic_acq_rel", AtomicAcqRelOrder},
            {"atomic_seq_cst", AtomicSeqCstOrder},
            {"atomic_load", AtomicLoad},
            {"atomic_store", AtomicStore},
            {"atomic_exchange", AtomicExchange},
            {"atomic_compare_exchange", AtomicCompareExchange},
            {"atomic_fetch_add", AtomicFetchAdd},
            {"atomic_fetch_sub", AtomicFetchSub},
            {"atomic_fetch_and", AtomicFetchAnd},
            {"atomic_fetch_or", AtomicFetchOr},
            {"atomic_fetch_xor", AtomicFetchXor},
            {"atomic_fence", AtomicFence},
        };
    if (const auto it = builtins.find(text); it != builtins.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto parse_symbol_case(std::string_view value, SourceRange range,
                       std::string_view key, std::string_view target_name)
    -> std::expected<ast::ExternSymbolCase, Diagnostic> {
    ast::ExternSymbolCase symbol_case;
    symbol_case.range = range;

    const auto symbol_separator = value.rfind(':');
    if (symbol_separator == std::string_view::npos || symbol_separator == 0 ||
        symbol_separator + 1 >= value.size()) {
        return std::unexpected(Diagnostic(
            "invalid " + std::string(key) +
                ": expected ret:TYPE,argN:TYPE:" + std::string(target_name),
            range));
    }

    const auto selectors = trim_ascii(value.substr(0, symbol_separator));
    const auto symbol_name =
        trim_ascii(value.substr(symbol_separator + std::size_t{1}));
    if (selectors.empty() || symbol_name.empty()) {
        return std::unexpected(Diagnostic(
            "invalid " + std::string(key) +
                ": expected ret:TYPE,argN:TYPE:" + std::string(target_name),
            range));
    }
    symbol_case.symbol_name = std::string(symbol_name);

    std::size_t clause_begin = 0;
    while (clause_begin < selectors.size()) {
        auto clause_end = selectors.find(',', clause_begin);
        if (clause_end == std::string_view::npos) {
            clause_end = selectors.size();
        }
        auto clause = trim_ascii(
            selectors.substr(clause_begin, clause_end - clause_begin));
        clause_begin = clause_end < selectors.size()
                           ? clause_end + std::size_t{1}
                           : selectors.size();
        if (clause.empty()) {
            continue;
        }

        const auto separator = clause.find(':');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= clause.size()) {
            return std::unexpected(
                Diagnostic("invalid " + std::string(key) +
                               " selector: expected ret:TYPE or argN:TYPE",
                           range));
        }

        const auto selector = trim_ascii(clause.substr(0, separator));
        const auto type_spec =
            trim_ascii(clause.substr(separator + std::size_t{1}));
        if (type_spec.empty()) {
            return std::unexpected(
                Diagnostic("invalid " + std::string(key) +
                               " selector: expected ret:TYPE or argN:TYPE",
                           range));
        }

        if (selector == "ret") {
            if (symbol_case.return_type_spec.has_value()) {
                return std::unexpected(Diagnostic(
                    "duplicate ret selector in " + std::string(key), range));
            }
            symbol_case.return_type_spec = std::string(type_spec);
            continue;
        }

        auto argument_index = parse_argument_index(selector);
        if (!argument_index) {
            return std::unexpected(
                Diagnostic("invalid " + std::string(key) +
                               " selector: expected ret:TYPE or argN:TYPE",
                           range));
        }
        symbol_case.argument_type_specs.emplace_back(*argument_index,
                                                     std::string(type_spec));
    }

    if (!symbol_case.return_type_spec.has_value() &&
        symbol_case.argument_type_specs.empty()) {
        return std::unexpected(Diagnostic(
            std::string(key) + " must specify at least one signature slot",
            range));
    }
    return symbol_case;
}

auto parse_lowering(const SourceFile& source, SourceRange range,
                    std::string_view body)
    -> std::expected<PendingLowering, Diagnostic> {
    PendingLowering lowering;
    lowering.range = range;
    lowering.target_offset = skip_decl_prefix(source.text(), range.end);

    body = trim_ascii(body);
    if (!body.starts_with("@lower")) {
        return std::unexpected(Diagnostic("unknown source directive", range));
    }
    body.remove_prefix(std::string_view("@lower").size());
    body = trim_ascii(body);

    while (!body.empty()) {
        const auto separator = body.find_first_of(" \t\r\n");
        const auto token = body.substr(0, separator);
        body = separator == std::string_view::npos
                   ? std::string_view()
                   : trim_ascii(body.substr(separator + 1));
        if (token.empty()) {
            continue;
        }

        const auto equals = token.find('=');
        if (equals == std::string_view::npos || equals == 0 ||
            equals + 1 >= token.size()) {
            return std::unexpected(Diagnostic(
                "expected source directive option in the form key=value",
                range));
        }

        const auto key = token.substr(0, equals);
        const auto value = token.substr(equals + 1);
        if (key == "intrinsic") {
            const auto is_typed_case =
                value.starts_with("ret:") || value.starts_with("arg");
            if (is_typed_case) {
                if (!lowering.intrinsic_name.empty()) {
                    return std::unexpected(Diagnostic(
                        "intrinsic lowering cannot mix bare intrinsic names "
                        "with typed intrinsic selectors",
                        range));
                }
                auto parsed_case =
                    parse_symbol_case(value, range, "intrinsic", "INTRINSIC");
                if (!parsed_case) {
                    return std::unexpected(parsed_case.error());
                }
                lowering.intrinsic_cases.push_back(std::move(*parsed_case));
                continue;
            }
            if (!lowering.intrinsic_cases.empty()) {
                return std::unexpected(Diagnostic(
                    "intrinsic lowering cannot mix bare intrinsic names with "
                    "typed intrinsic selectors",
                    range));
            }
            if (!lowering.intrinsic_name.empty()) {
                return std::unexpected(
                    Diagnostic("duplicate intrinsic lowering name", range));
            }
            lowering.intrinsic_name = std::string(value);
            continue;
        }
        if (key == "constant") {
            if (value == "null") {
                lowering.constant_kind = ast::LoweringConstantKind::NullValue;
                continue;
            }
            return std::unexpected(Diagnostic("unknown lowering constant '" +
                                                  std::string(value) + "'",
                                              range));
        }
        if (key == "builtin") {
            const auto builtin_kind = parse_builtin_lowering_kind(value);
            if (!builtin_kind.has_value()) {
                return std::unexpected(Diagnostic("unknown lowering builtin '" +
                                                      std::string(value) + "'",
                                                  range));
            }
            lowering.builtin_kind = *builtin_kind;
            continue;
        }
        if (key == "link_case") {
            auto parsed_case =
                parse_symbol_case(value, range, "link_case", "SYMBOL");
            if (!parsed_case) {
                return std::unexpected(parsed_case.error());
            }
            lowering.extern_symbol_cases.push_back(std::move(*parsed_case));
            continue;
        }
        if (key == "llvm_return") {
            lowering.intrinsic_return_spec = std::string(value);
            continue;
        }
        if (key == "return") {
            auto parsed = parse_return_argument_index(value);
            if (!parsed) {
                return std::unexpected(Diagnostic(
                    "invalid lowering return policy: " + parsed.error(),
                    range));
            }
            lowering.return_argument_index = *parsed;
            continue;
        }
        if (key.starts_with("arg")) {
            auto parsed = parse_argument_index(key);
            if (!parsed) {
                return std::unexpected(Diagnostic(
                    "invalid lowering argument slot: " + parsed.error(),
                    range));
            }
            lowering.argument_overrides.push_back(
                ast::IntrinsicLoweringArgument{
                    .index = *parsed, .value_spec = std::string(value)});
            continue;
        }
        return std::unexpected(Diagnostic("unknown source directive option '" +
                                              std::string(key) + "'",
                                          range));
    }

    const auto has_intrinsic =
        !lowering.intrinsic_name.empty() || !lowering.intrinsic_cases.empty();
    const auto has_constant =
        lowering.constant_kind != ast::LoweringConstantKind::None;
    const auto has_builtin =
        lowering.builtin_kind != ast::BuiltinCallKind::None;
    const auto has_link_cases = !lowering.extern_symbol_cases.empty();
    const auto lowering_modes =
        static_cast<int>(has_intrinsic) + static_cast<int>(has_constant) +
        static_cast<int>(has_builtin) + static_cast<int>(has_link_cases);
    if (lowering_modes != 1) {
        return std::unexpected(Diagnostic(
            "lowering directive requires exactly one of intrinsic=..., "
            "constant=..., builtin=..., or link_case=...",
            range));
    }
    if ((has_constant || has_builtin || has_link_cases) &&
        (lowering.intrinsic_return_spec.has_value() ||
         lowering.return_argument_index.has_value() ||
         !lowering.argument_overrides.empty())) {
        return std::unexpected(Diagnostic(
            "constant, builtin, and link_case lowerings do not support "
            "llvm_return=..., return=..., or argN=...",
            range));
    }
    if (lowering.target_offset >= source.text().size()) {
        return std::unexpected(Diagnostic(
            "lowering directive must be followed by a declaration", range));
    }
    return lowering;
}

auto collect_lowerings(const SourceFile& source)
    -> std::pair<std::vector<PendingLowering>, DiagnosticList> {
    std::vector<PendingLowering> lowerings;
    DiagnosticList diagnostics;

    const auto text = source.text();
    std::size_t line_begin = 0;
    while (line_begin < text.size()) {
        auto line_end = text.find('\n', line_begin);
        if (line_end == std::string_view::npos) {
            line_end = text.size();
        }

        const auto line = text.substr(line_begin, line_end - line_begin);
        const auto trimmed = trim_ascii(line);
        if (trimmed.starts_with("//")) {
            const auto body = trim_ascii(trimmed.substr(2));
            if (body.starts_with("@lower")) {
                auto parsed = parse_lowering(
                    source, source.range(line_begin, line_end), body);
                if (!parsed) {
                    diagnostics.push_back(parsed.error());
                } else {
                    lowerings.push_back(std::move(*parsed));
                }
            }
        }

        line_begin = line_end < text.size() ? line_end + 1 : text.size();
    }

    return {std::move(lowerings), std::move(diagnostics)};
}

auto apply_lowerings(ast::Module& module,
                     const std::vector<PendingLowering>& lowerings)
    -> DiagnosticList {
    DiagnosticList diagnostics;
    for (const auto& lowering : lowerings) {
        ast::Decl* target_decl = nullptr;
        for (auto& decl : module.declarations) {
            const auto decl_begin =
                std::visit([](auto& value) { return value.range.begin; }, decl);
            if (decl_begin != lowering.target_offset) {
                continue;
            }
            target_decl = &decl;
            break;
        }

        if (target_decl == nullptr) {
            diagnostics.push_back(Diagnostic(
                "lowering directive must immediately precede a declaration",
                lowering.range));
            continue;
        }

        auto* function = std::get_if<ast::FunctionDecl>(target_decl);
        if (function == nullptr) {
            diagnostics.push_back(Diagnostic(
                "lowering directives may only be attached to functions",
                lowering.range));
            continue;
        }
        if (function->intrinsic_lowering.has_value() ||
            function->builtin_lowering.has_value() ||
            !function->extern_symbol_cases.empty()) {
            diagnostics.push_back(
                Diagnostic("duplicate lowering directive for function '" +
                               function->name + "'",
                           lowering.range));
            continue;
        }

        if (lowering.builtin_kind != ast::BuiltinCallKind::None) {
            function->builtin_lowering = ast::BuiltinLowering{
                .range = lowering.range,
                .builtin_kind = lowering.builtin_kind,
            };
        } else if (!lowering.extern_symbol_cases.empty()) {
            function->extern_symbol_cases = lowering.extern_symbol_cases;
        } else {
            function->intrinsic_lowering = ast::IntrinsicLowering{
                .range = lowering.range,
                .intrinsic_name = lowering.intrinsic_name,
                .constant_kind = lowering.constant_kind,
                .intrinsic_return_spec = lowering.intrinsic_return_spec,
                .return_argument_index = lowering.return_argument_index,
                .argument_overrides = lowering.argument_overrides,
                .intrinsic_cases = lowering.intrinsic_cases,
            };
        }
    }
    return diagnostics;
}

auto is_within_root(const std::filesystem::path& path,
                    const std::filesystem::path& root_dir) -> bool {
    const auto relative = path.lexically_relative(root_dir);
    if (relative.empty()) {
        return false;
    }
    if (relative.is_absolute()) {
        return false;
    }
    const auto begin = relative.begin();
    return begin == relative.end() || begin->string() != "..";
}

auto module_name_from_relative_path(const std::filesystem::path& relative_path)
    -> std::string {
    auto module_path = relative_path;
    module_path.replace_extension();

    std::string module_name;
    for (const auto& part : module_path) {
        const auto piece = part.string();
        if (piece.empty() || piece == ".") {
            continue;
        }
        if (!module_name.empty()) {
            module_name.push_back('.');
        }
        module_name += piece;
    }
    return module_name;
}

auto dotted_tail_to_path(std::string_view module_name)
    -> std::filesystem::path {
    std::filesystem::path path;
    std::string current_part;
    for (const auto ch : module_name) {
        if (ch == '.') {
            if (!current_part.empty()) {
                path /= current_part;
                current_part.clear();
            }
            continue;
        }
        current_part.push_back(ch);
    }
    if (!current_part.empty()) {
        path /= current_part;
    }
    path.replace_extension(".cyan");
    return path;
}

auto describe_import_path(const ast::ImportDecl& import_decl) -> std::string {
    std::string path;
    if (import_decl.is_builtin) {
        path.push_back('/');
    } else {
        for (std::size_t index = 0; index < import_decl.parent_depth; ++index) {
            path += "..";
        }
    }
    path += import_decl.module_name;
    return path;
}

auto parse_source(const SourceFile& source)
    -> std::expected<ast::Module, DiagnosticList> {
    Lexer lexer(source);
    auto tokens = lexer.lexAll();
    if (!tokens) {
        return std::unexpected(DiagnosticList{tokens.error()});
    }

    Parser parser(source, std::move(*tokens));
    auto module = parser.parseModule();
    auto [lowerings, lowering_diagnostics] = collect_lowerings(source);
    module.diagnostics.insert(module.diagnostics.end(),
                              lowering_diagnostics.begin(),
                              lowering_diagnostics.end());
    auto apply_diagnostics = apply_lowerings(module, lowerings);
    module.diagnostics.insert(module.diagnostics.end(),
                              apply_diagnostics.begin(),
                              apply_diagnostics.end());
    return module;
}

auto resolve_module_name(const ast::Package& package,
                         const std::filesystem::path& canonical_path)
    -> std::expected<std::string, std::string> {
    if (is_within_root(canonical_path, builtin_root_path())) {
        const auto relative =
            canonical_path.lexically_relative(builtin_root_path());
        return module_name_from_relative_path(relative);
    }

    if (!is_within_root(canonical_path, package.root_dir)) {
        return std::unexpected("module escapes the package root");
    }

    const auto relative = canonical_path.lexically_relative(package.root_dir);
    return module_name_from_relative_path(relative);
}

auto load_source(const std::filesystem::path& canonical_path,
                 const ModuleLoadOptions& options)
    -> std::expected<SourceFile, std::string> {
    if (options.overrides != nullptr) {
        if (const auto it = options.overrides->find(path_key(canonical_path));
            it != options.overrides->end()) {
            return SourceFile::fromText(canonical_path, it->second);
        }
    }

    if (is_within_root(canonical_path, builtin_root_path())) {
        const auto relative =
            canonical_path.lexically_relative(builtin_root_path());
        const auto module_name = module_name_from_relative_path(relative);
        const auto& builtins = builtin_module_sources();
        if (const auto it = builtins.find(module_name); it != builtins.end()) {
            return SourceFile::fromText(canonical_path,
                                        std::string(it->second));
        }
        return std::unexpected("unknown builtin module");
    }
    return SourceFile::load(canonical_path);
}

auto resolve_import_path(const ast::Package& package, const ast::Module& module,
                         const ast::ImportDecl& import_decl)
    -> std::expected<std::filesystem::path, std::string> {
    if (import_decl.module_name.empty()) {
        return std::unexpected("expected imported module name");
    }

    if (import_decl.is_builtin) {
        auto path =
            builtin_root_path() / dotted_tail_to_path(import_decl.module_name);
        return normalized_path(path);
    }

    auto base_dir = module.path.parent_path();
    for (std::size_t index = 0; index < import_decl.parent_depth; ++index) {
        base_dir = base_dir.parent_path();
    }
    auto path = normalized_path(base_dir /
                                dotted_tail_to_path(import_decl.module_name));
    const auto& root_dir =
        module.is_builtin ? builtin_root_path() : package.root_dir;
    if (!is_within_root(path, root_dir)) {
        return std::unexpected(
            module.is_builtin ? "builtin module import escapes the builtin root"
                              : "import escapes the package root");
    }
    return path;
}

auto load_module(ast::Package& package,
                 const std::filesystem::path& module_path,
                 std::unordered_map<std::string, ast::Module*>& loaded_modules,
                 const ModuleLoadOptions& options)
    -> std::expected<ast::Module*, DiagnosticList> {
    const auto canonical_path = normalized_path(module_path);
    const auto key = path_key(canonical_path);
    if (const auto it = loaded_modules.find(key); it != loaded_modules.end()) {
        return it->second;
    }

    auto source = load_source(canonical_path, options);
    if (!source) {
        return std::unexpected(DiagnosticList{Diagnostic(source.error())});
    }
    package.sources.push_back(std::make_unique<SourceFile>(std::move(*source)));
    auto* source_file = package.sources.back().get();

    auto parsed_module = parse_source(*source_file);
    if (!parsed_module) {
        return std::unexpected(parsed_module.error());
    }

    auto module_name = resolve_module_name(package, canonical_path);
    if (!module_name) {
        return std::unexpected(DiagnosticList{Diagnostic(module_name.error())});
    }

    auto stored_module =
        std::make_unique<ast::Module>(std::move(*parsed_module));
    stored_module->source = source_file;
    stored_module->path = canonical_path;
    stored_module->module_name = std::move(*module_name);
    stored_module->is_builtin =
        is_within_root(canonical_path, builtin_root_path());
    auto* module = stored_module.get();
    package.modules.push_back(std::move(stored_module));
    loaded_modules.emplace(key, module);

    for (auto& import_decl : module->imports) {
        auto imported_path = resolve_import_path(package, *module, import_decl);
        if (!imported_path) {
            return std::unexpected(DiagnosticList{
                Diagnostic("failed to resolve imported module '" +
                               describe_import_path(import_decl) +
                               "': " + imported_path.error(),
                           import_decl.range),
            });
        }
        if (*imported_path == canonical_path) {
            return std::unexpected(DiagnosticList{
                Diagnostic("module cannot import itself", import_decl.range),
            });
        }
        auto imported_module =
            load_module(package, *imported_path, loaded_modules, options);
        if (!imported_module) {
            if (!imported_module.error().empty() &&
                imported_module.error().front().hasRange()) {
                return std::unexpected(imported_module.error());
            }
            const auto message =
                imported_module.error().empty()
                    ? "unknown error"
                    : imported_module.error().front().message();
            return std::unexpected(DiagnosticList{
                Diagnostic("failed to load imported module '" +
                               describe_import_path(import_decl) +
                               "': " + message,
                           import_decl.range),
            });
        }
        import_decl.imported_module = *imported_module;
    }

    return module;
}

} // namespace

auto normalized_path(const std::filesystem::path& path)
    -> std::filesystem::path {
    return std::filesystem::absolute(path).lexically_normal();
}

auto path_key(const std::filesystem::path& path) -> std::string {
    return normalized_path(path).string();
}

auto builtin_virtual_root_path() -> const std::filesystem::path& {
    return builtin_root_path();
}

auto builtin_source_text(const std::filesystem::path& path)
    -> std::optional<std::string> {
    const auto canonical_path = normalized_path(path);
    if (!is_within_root(canonical_path, builtin_root_path())) {
        return std::nullopt;
    }
    const auto relative =
        canonical_path.lexically_relative(builtin_root_path());
    const auto module_name = module_name_from_relative_path(relative);
    const auto& builtins = builtin_module_sources();
    const auto it = builtins.find(module_name);
    if (it == builtins.end()) {
        return std::nullopt;
    }
    return std::string(it->second);
}

auto load_package(ast::Package& package,
                  const std::filesystem::path& entry_path,
                  const ModuleLoadOptions& options)
    -> std::expected<void, DiagnosticList> {
    std::unordered_map<std::string, ast::Module*> loaded_modules;

    const auto canonical_entry = normalized_path(entry_path);
    package.root_dir = canonical_entry.parent_path();

    auto entry_module =
        load_module(package, canonical_entry, loaded_modules, options);
    if (!entry_module) {
        return std::unexpected(entry_module.error());
    }
    package.entry_module = *entry_module;
    return {};
}

auto collect_package_diagnostics(const ast::Package& package)
    -> DiagnosticList {
    DiagnosticList diagnostics;
    for (const auto& module : package.modules) {
        diagnostics.insert(diagnostics.end(), module->diagnostics.begin(),
                           module->diagnostics.end());
    }
    return diagnostics;
}

} // namespace cyan

#include "cyan/module_loader.hpp"

#include "cyan/lexer.hpp"
#include "cyan/parser.hpp"
#include "cyan/source.hpp"

#include <algorithm>
#include <expected>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cyan {

namespace {

constexpr std::string_view BUILTIN_ROOT_TEXT = "/__cyan_builtin__";

auto builtin_root_path() -> const std::filesystem::path& {
    static const auto path = std::filesystem::path(BUILTIN_ROOT_TEXT);
    return path;
}

auto builtin_module_sources()
    -> const std::unordered_map<std::string_view, std::string_view>& {
    static const auto modules =
        std::unordered_map<std::string_view, std::string_view>{
            {
                "std.heap",
                R"(export extern {
    void* malloc(i64 size);
    void free(void* ptr);
    void* realloc(void* ptr, i64 size);
}

export T* malloc_array<T>(i64 count) {
    unchecked {
        return malloc(count * sizeof(T)) as T*;
    }
}

export T* realloc_array<T>(T* ptr, i64 count) {
    unchecked {
        return realloc(ptr as void*, count * sizeof(T)) as T*;
    }
}

export void free_ptr<T>(T* ptr) {
    unchecked {
        free(ptr as void*);
    }
}
)",
            },
            {
                "std.mem",
                R"(export extern {
    i32 memcmp(const char* lhs, const char* rhs, i64 size);
}

export char* memcpy(char* dst, const char* src, i64 size) {
    return dst;
}

export char* memset(char* dst, u8 value, i64 size) {
    return dst;
}

export T* copy<T>(T* dst, const T* src, i64 count) {
    unchecked {
        memcpy(dst as char*, src as const char*, count * sizeof(T));
    }
    return dst;
}

export T* fill<T>(T* dst, u8 value, i64 count) {
    unchecked {
        memset(dst as char*, value, count * sizeof(T));
    }
    return dst;
}
)",
            },
            {
                "std.cstr",
                R"(export extern {
    i64 strlen(const char* text);
    i32 strcmp(const char* lhs, const char* rhs);
}
)",
            },
            {
                "std.file",
                R"(export extern {
    void* fopen(const char* path, const char* mode);
    i32 fclose(void* file);
    i64 fread(char* ptr, i64 size, i64 count, void* file);
    i64 fwrite(const char* ptr, i64 size, i64 count, void* file);
    i32 fseek(void* file, i64 offset, i32 whence);
    i64 ftell(void* file);
}
)",
            },
            {
                "std.slice",
                R"(export i64 span_len<T>([]T values) {
    return len(values);
}

export []T span_slice<T>([]T values, i64 start, i64 count) {
    return subslice(values, start, count);
}
)",
            },
        };
    return modules;
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
    return parser.parseModule();
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

    if (options.overrides != nullptr) {
        if (const auto it = options.overrides->find(path_key(canonical_path));
            it != options.overrides->end()) {
            return SourceFile::fromText(canonical_path, it->second);
        }
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

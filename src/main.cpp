#include "cyan/codegen.hpp"
#include "cyan/diagnostic.hpp"
#include "cyan/lexer.hpp"
#include "cyan/lsp_support.hpp"
#include "cyan/parser.hpp"
#include "cyan/sema.hpp"
#include "cyan/source.hpp"
#include "cyan/type.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyan {

struct CommandLine {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    OutputKind output_kind = OutputKind::Object;
    CodegenOptions codegen_options;
    bool check_only = false;
    bool lsp_mode = false;
};

auto parse_optimization_level(std::string_view arg)
    -> std::optional<OptimizationLevel> {
    if (arg == "-O0") {
        return OptimizationLevel::O0;
    }
    if (arg == "-O1") {
        return OptimizationLevel::O1;
    }
    if (arg == "-O2") {
        return OptimizationLevel::O2;
    }
    if (arg == "-O3") {
        return OptimizationLevel::O3;
    }
    if (arg == "-Ofast") {
        return OptimizationLevel::Ofast;
    }
    return std::nullopt;
}

auto parse_command_line(int argc, char** argv) -> std::optional<CommandLine> {
    CommandLine options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--check") {
            options.check_only = true;
            continue;
        }
        if (arg == "--lsp") {
            options.lsp_mode = true;
            continue;
        }
        if (arg == "--emit-llvm") {
            options.output_kind = OutputKind::LLVMIR;
            continue;
        }
        if (arg == "-g") {
            options.codegen_options.emit_debug_info = true;
            continue;
        }
        if (arg.starts_with("-O")) {
            const auto level = parse_optimization_level(arg);
            if (!level.has_value()) {
                std::cerr << "unsupported optimization option: " << arg
                          << '\n';
                return std::nullopt;
            }
            options.codegen_options.optimization_level = *level;
            continue;
        }
        if (arg == "-o") {
            if (index + 1 >= argc) {
                std::cerr << "missing output path after -o\n";
                return std::nullopt;
            }
            options.output_path = argv[++index];
            continue;
        }
        if (!options.input_path.empty()) {
            std::cerr << "unexpected extra input: " << arg << '\n';
            return std::nullopt;
        }
        options.input_path = arg;
    }

    if (options.lsp_mode) {
        if (!options.input_path.empty()) {
            std::cerr << "--lsp does not take an input file\n";
            return std::nullopt;
        }
        return options;
    }

    if (options.input_path.empty()) {
        std::cerr
            << "usage: cyan <input.cyan> [--check] [--emit-llvm] "
               "[-O0|-O1|-O2|-O3|-Ofast] [-g] [-o output]\n"
            << "       cyan --lsp\n";
        return std::nullopt;
    }

    if (options.output_path.empty() && !options.check_only) {
        options.output_path = options.input_path;
        options.output_path.replace_extension(
            options.output_kind == OutputKind::LLVMIR ? ".ll" : ".o");
    }

    return options;
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

auto module_path_from_name(const std::filesystem::path& root_dir,
                           std::string_view module_name)
    -> std::filesystem::path {
    std::filesystem::path path = root_dir;
    std::string current_part;
    for (const auto ch : module_name) {
        if (ch == '.') {
            path /= current_part;
            current_part.clear();
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

auto collect_package_diagnostics(const ast::Package& package)
    -> DiagnosticList {
    DiagnosticList diagnostics;
    for (const auto& module : package.modules) {
        diagnostics.insert(diagnostics.end(), module->diagnostics.begin(),
                           module->diagnostics.end());
    }
    return diagnostics;
}

auto load_module(ast::Package& package, const std::filesystem::path& root_dir,
                 const std::filesystem::path& module_path,
                 std::string module_name,
                 std::unordered_map<std::string, ast::Module*>& loaded_modules)
    -> std::expected<ast::Module*, DiagnosticList> {
    const auto canonical_path =
        std::filesystem::absolute(module_path).lexically_normal();
    const auto key = canonical_path.string();
    if (const auto it = loaded_modules.find(key); it != loaded_modules.end()) {
        return it->second;
    }

    auto source = SourceFile::load(canonical_path);
    if (!source) {
        return std::unexpected(DiagnosticList{Diagnostic(source.error())});
    }
    package.sources.push_back(std::make_unique<SourceFile>(std::move(*source)));
    auto* source_file = package.sources.back().get();

    auto parsed_module = parse_source(*source_file);
    if (!parsed_module) {
        return std::unexpected(parsed_module.error());
    }

    auto stored_module =
        std::make_unique<ast::Module>(std::move(*parsed_module));
    stored_module->source = source_file;
    stored_module->path = canonical_path;
    stored_module->module_name = std::move(module_name);
    auto* module = stored_module.get();
    package.modules.push_back(std::move(stored_module));
    loaded_modules.emplace(key, module);

    for (auto& import_decl : module->imports) {
        const auto imported_path =
            module_path_from_name(root_dir, import_decl.module_name);
        if (std::filesystem::absolute(imported_path).lexically_normal() ==
            canonical_path) {
            return std::unexpected(DiagnosticList{
                Diagnostic("module cannot import itself", import_decl.range)});
        }
        auto imported_module =
            load_module(package, root_dir, imported_path,
                        import_decl.module_name, loaded_modules);
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
                               import_decl.module_name + "': " + message,
                           import_decl.range)});
        }
        import_decl.imported_module = *imported_module;
    }

    return module;
}

auto load_package(ast::Package& package,
                  const std::filesystem::path& entry_path)
    -> std::expected<void, DiagnosticList> {
    std::unordered_map<std::string, ast::Module*> loaded_modules;

    const auto canonical_entry =
        std::filesystem::absolute(entry_path).lexically_normal();
    const auto root_dir = canonical_entry.parent_path();
    const auto module_name =
        module_name_from_relative_path(canonical_entry.filename());

    auto entry_module = load_module(package, root_dir, canonical_entry,
                                    module_name, loaded_modules);
    if (!entry_module) {
        return std::unexpected(entry_module.error());
    }
    package.entry_module = *entry_module;
    return {};
}

} // namespace cyan

auto main(int argc, char** argv) -> int {
    const auto options = cyan::parse_command_line(argc, argv);
    if (!options.has_value()) {
        return EXIT_FAILURE;
    }

    if (options->lsp_mode) {
        cyan::LanguageServer server;
        return server.run(std::cin, std::cout);
    }

    cyan::ast::Package package;
    auto loaded = cyan::load_package(package, options->input_path);
    if (!loaded) {
        auto diagnostics = cyan::collect_package_diagnostics(package);
        diagnostics.insert(diagnostics.end(), loaded.error().begin(),
                           loaded.error().end());
        cyan::print_diagnostics(std::cerr, diagnostics);
        return EXIT_FAILURE;
    }

    auto diagnostics = cyan::collect_package_diagnostics(package);
    cyan::TypeContext types;
    cyan::SemanticAnalyzer sema(types);
    auto analysis = sema.analyze(package);
    diagnostics.insert(diagnostics.end(), analysis.diagnostics.begin(),
                       analysis.diagnostics.end());
    if (!diagnostics.empty()) {
        cyan::print_diagnostics(std::cerr, diagnostics);
        return EXIT_FAILURE;
    }

    if (options->check_only) {
        return EXIT_SUCCESS;
    }

    cyan::CodeGenerator codegen(types);
    auto emitted = codegen.emit(package, options->output_path,
                                options->output_kind,
                                options->codegen_options);
    if (!emitted) {
        cyan::print_diagnostic(std::cerr, emitted.error());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

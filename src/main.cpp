#include "cyan/codegen.hpp"
#include "cyan/diagnostic.hpp"
#include "cyan/lsp_support.hpp"
#include "cyan/module_loader.hpp"
#include "cyan/sema.hpp"
#include "cyan/type.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
                std::cerr << "unsupported optimization option: " << arg << '\n';
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
        std::cerr << "usage: cyan <input.cyan> [--check] [--emit-llvm] "
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
                                options->output_kind, options->codegen_options);
    if (!emitted) {
        cyan::print_diagnostic(std::cerr, emitted.error());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

#pragma once

#include "cyan/ast.hpp"
#include "cyan/diagnostic.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace cyan {

using SourceOverrideMap = std::unordered_map<std::string, std::string>;

struct ModuleLoadOptions {
    const SourceOverrideMap* overrides = nullptr;
};

[[nodiscard]] auto normalized_path(const std::filesystem::path& path)
    -> std::filesystem::path;
[[nodiscard]] auto path_key(const std::filesystem::path& path) -> std::string;
auto load_package(ast::Package& package,
                  const std::filesystem::path& entry_path,
                  const ModuleLoadOptions& options = {})
    -> std::expected<void, DiagnosticList>;
[[nodiscard]] auto collect_package_diagnostics(const ast::Package& package)
    -> DiagnosticList;

} // namespace cyan

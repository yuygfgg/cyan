#pragma once

#include "sc/ast.hpp"
#include "sc/diagnostic.hpp"
#include "sc/type.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

namespace sc {

enum class OutputKind : std::uint8_t {
    Object,
    LLVMIR,
};

enum class OptimizationLevel : std::uint8_t {
    O0,
    O1,
    O2,
    O3,
    Ofast,
};

struct CodegenOptions {
    OptimizationLevel optimization_level = OptimizationLevel::O0;
    bool emit_debug_info = false;
};

class CodeGenerator {
  public:
    explicit CodeGenerator(TypeContext& types);

    auto emit(ast::Package& package, const std::filesystem::path& output_path,
              OutputKind output_kind, const CodegenOptions& options = {})
        -> std::expected<void, Diagnostic>;

  private:
    TypeContext& types;
};

} // namespace sc

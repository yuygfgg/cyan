#include "cyan/codegen.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>

namespace cyan {

namespace {

template <typename... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

auto to_llvm_optimization_level(OptimizationLevel level)
    -> llvm::OptimizationLevel {
    switch (level) {
    case OptimizationLevel::O0:
        return llvm::OptimizationLevel::O0;
    case OptimizationLevel::O1:
        return llvm::OptimizationLevel::O1;
    case OptimizationLevel::O2:
        return llvm::OptimizationLevel::O2;
    case OptimizationLevel::O3:
    case OptimizationLevel::Ofast:
        return llvm::OptimizationLevel::O3;
    }
    return llvm::OptimizationLevel::O0;
}

auto to_llvm_code_gen_opt_level(OptimizationLevel level)
    -> llvm::CodeGenOptLevel {
    switch (level) {
    case OptimizationLevel::O0:
        return llvm::CodeGenOptLevel::None;
    case OptimizationLevel::O1:
        return llvm::CodeGenOptLevel::Less;
    case OptimizationLevel::O2:
        return llvm::CodeGenOptLevel::Default;
    case OptimizationLevel::O3:
    case OptimizationLevel::Ofast:
        return llvm::CodeGenOptLevel::Aggressive;
    }
    return llvm::CodeGenOptLevel::None;
}

auto optimization_flag_string(OptimizationLevel level) -> std::string_view {
    switch (level) {
    case OptimizationLevel::O0:
        return "-O0";
    case OptimizationLevel::O1:
        return "-O1";
    case OptimizationLevel::O2:
        return "-O2";
    case OptimizationLevel::O3:
        return "-O3";
    case OptimizationLevel::Ofast:
        return "-Ofast";
    }
    return "-O0";
}

class LLVMCodegen {
  public:
    LLVMCodegen(TypeContext& types, CodegenOptions options)
        : types(types), options(options), module("cyan_module", context),
          builder(context) {}

    auto emit(ast::Package& package, const std::filesystem::path& output_path,
              OutputKind output_kind) -> std::expected<void, Diagnostic> {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        module.setTargetTriple(
            llvm::Triple(llvm::sys::getDefaultTargetTriple()));
        std::string error;
        const auto* target =
            llvm::TargetRegistry::lookupTarget(module.getTargetTriple(), error);
        if (target == nullptr) {
            return std::unexpected(Diagnostic(error));
        }

        module.setSourceFileName(package.entry_module != nullptr
                                     ? package.entry_module->path.string()
                                     : output_path.string());

        llvm::TargetOptions target_options;
        if (options.optimization_level == OptimizationLevel::Ofast) {
            target_options.NoInfsFPMath = true;
            target_options.NoNaNsFPMath = true;
            target_options.NoSignedZerosFPMath = true;
            target_options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
        }

        auto target_machine =
            std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
                module.getTargetTriple(), "generic", "", target_options, {},
                std::nullopt,
                to_llvm_code_gen_opt_level(options.optimization_level)));
        module.setDataLayout(target_machine->createDataLayout());

        auto declared = declareStructs(package);
        if (!declared) {
            return std::unexpected(declared.error());
        }
        initializeDebugInfo(package);
        auto lowered = declareFunctions(package);
        if (!lowered) {
            return std::unexpected(lowered.error());
        }
        auto generated = emitFunctions(package);
        if (!generated) {
            return std::unexpected(generated.error());
        }

        finalizeDebugInfo();
        auto optimized = optimizeModule(*target_machine);
        if (!optimized) {
            return std::unexpected(optimized.error());
        }

        if (llvm::verifyModule(module, &llvm::errs())) {
            return std::unexpected(
                Diagnostic("generated LLVM module is invalid"));
        }

        if (output_kind == OutputKind::LLVMIR) {
            std::error_code code;
            llvm::raw_fd_ostream stream(output_path.string(), code,
                                        llvm::sys::fs::OF_Text);
            if (code) {
                return std::unexpected(Diagnostic(code.message()));
            }
            module.print(stream, nullptr);
            return {};
        }

        std::error_code code;
        llvm::raw_fd_ostream stream(output_path.string(), code,
                                    llvm::sys::fs::OF_None);
        if (code) {
            return std::unexpected(Diagnostic(code.message()));
        }
        llvm::legacy::PassManager pass_manager;
        if (target_machine->addPassesToEmitFile(
                pass_manager, stream, nullptr,
                llvm::CodeGenFileType::ObjectFile)) {
            return std::unexpected(
                Diagnostic("target machine cannot emit an object file"));
        }
        pass_manager.run(module);
        stream.flush();
        return {};
    }

  private:
    struct EnumLayout {
        llvm::StructType* type = nullptr;
        std::vector<const Type*> payload_types;
        std::uint64_t payload_size = 0;
        std::uint64_t payload_padding = 0;
    };

    struct LocalSlot {
        llvm::Value* address = nullptr;
        const Type* type = nullptr;
        llvm::Value* initialized_flag = nullptr;
    };

    struct FunctionFrame {
        std::unordered_map<std::size_t, LocalSlot> locals;
    };

    class ScopedDebugLocation {
      public:
        ScopedDebugLocation(LLVMCodegen& codegen, SourceRange range)
            : builder(codegen.builder),
              previous(builder.getCurrentDebugLocation()) {
            if (const auto current = codegen.makeDebugLocation(range);
                current) {
                builder.SetCurrentDebugLocation(current);
            }
        }

        ~ScopedDebugLocation() { builder.SetCurrentDebugLocation(previous); }

      private:
        llvm::IRBuilder<>& builder;
        llvm::DebugLoc previous;
    };

    class ScopedDebugScope {
      public:
        ScopedDebugScope(LLVMCodegen& codegen, SourceRange range)
            : codegen(codegen), active(codegen.pushDebugScope(range)) {}

        ~ScopedDebugScope() {
            if (active) {
                codegen.popDebugScope();
            }
        }

      private:
        LLVMCodegen& codegen;
        bool active = false;
    };

    [[nodiscard]] auto debugInfoEnabled() const -> bool {
        return di_builder != nullptr;
    }

    [[nodiscard]] auto optimizationEnabled() const -> bool {
        return options.optimization_level != OptimizationLevel::O0;
    }

    [[nodiscard]] auto currentDebugScope() const -> llvm::DIScope* {
        if (!debug_scope_stack.empty()) {
            return debug_scope_stack.back();
        }
        return current_subprogram;
    }

    auto makeDebugLocation(SourceRange range) -> llvm::DebugLoc {
        if (!debugInfoEnabled() || range.source == nullptr ||
            currentDebugScope() == nullptr) {
            return {};
        }
        const auto location = range.source->location(range.begin);
        return llvm::DILocation::get(
            context, static_cast<unsigned>(location.line),
            static_cast<unsigned>(location.column), currentDebugScope());
    }

    auto pushDebugScope(SourceRange range) -> bool {
        if (!debugInfoEnabled() || range.source == nullptr ||
            currentDebugScope() == nullptr) {
            return false;
        }
        const auto location = range.source->location(range.begin);
        auto* lexical_block = di_builder->createLexicalBlock(
            currentDebugScope(), getOrCreateDebugFile(*range.source),
            static_cast<unsigned>(location.line),
            static_cast<unsigned>(location.column));
        debug_scope_stack.push_back(lexical_block);
        return true;
    }

    auto popDebugScope() -> void {
        if (!debug_scope_stack.empty()) {
            debug_scope_stack.pop_back();
        }
    }

    [[nodiscard]] auto debugFlagsString() const -> std::string {
        std::string flags(optimization_flag_string(options.optimization_level));
        if (options.emit_debug_info) {
            if (!flags.empty()) {
                flags.push_back(' ');
            }
            flags += "-g";
        }
        return flags;
    }

    auto initializeDebugInfo(ast::Package& package) -> void {
        if (!options.emit_debug_info || package.entry_module == nullptr ||
            package.entry_module->source == nullptr) {
            return;
        }

        module.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                             llvm::DEBUG_METADATA_VERSION);
        module.addModuleFlag(llvm::Module::Warning, "Dwarf Version",
                             llvm::dwarf::DWARF_VERSION);

        di_builder = std::make_unique<llvm::DIBuilder>(module);
        auto* file = getOrCreateDebugFile(*package.entry_module->source);
        compile_unit = di_builder->createCompileUnit(
            llvm::DISourceLanguageName(llvm::dwarf::DW_LANG_C), file, "Cyan",
            optimizationEnabled(), debugFlagsString(), 0);
    }

    auto finalizeDebugInfo() -> void {
        if (!debugInfoEnabled()) {
            return;
        }
        builder.SetCurrentDebugLocation({});
        debug_scope_stack.clear();
        current_subprogram = nullptr;
        di_builder->finalize();
    }

    auto optimizeModule(llvm::TargetMachine& target_machine)
        -> std::expected<void, Diagnostic> {
        if (!optimizationEnabled()) {
            return {};
        }

        llvm::LoopAnalysisManager loop_analysis_manager;
        llvm::FunctionAnalysisManager function_analysis_manager;
        llvm::CGSCCAnalysisManager cgscc_analysis_manager;
        llvm::ModuleAnalysisManager module_analysis_manager;
        llvm::PassBuilder pass_builder(&target_machine);

        target_machine.registerPassBuilderCallbacks(pass_builder);
        pass_builder.registerLoopAnalyses(loop_analysis_manager);
        pass_builder.registerFunctionAnalyses(function_analysis_manager);
        pass_builder.registerCGSCCAnalyses(cgscc_analysis_manager);
        pass_builder.registerModuleAnalyses(module_analysis_manager);
        pass_builder.crossRegisterProxies(
            loop_analysis_manager, function_analysis_manager,
            cgscc_analysis_manager, module_analysis_manager);

        auto module_pass_manager = pass_builder.buildPerModuleDefaultPipeline(
            to_llvm_optimization_level(options.optimization_level));
        module_pass_manager.run(module, module_analysis_manager);
        return {};
    }

    auto getOrCreateDebugFile(const SourceFile& source) -> llvm::DIFile* {
        if (const auto it = debug_files.find(&source);
            it != debug_files.end()) {
            return it->second;
        }

        const auto& path = source.path();
        const auto filename =
            path.filename().empty() ? path.string() : path.filename().string();
        auto* file =
            di_builder->createFile(filename, path.parent_path().string());
        debug_files.emplace(&source, file);
        return file;
    }

    auto getDebugType(const Type* type) -> llvm::DIType* {
        if (!debugInfoEnabled() || type == nullptr) {
            return nullptr;
        }
        type = types.unqualify(type);
        if (const auto it = debug_types.find(type); it != debug_types.end()) {
            return it->second;
        }

        const auto& data_layout = module.getDataLayout();
        switch (type->kind) {
        case TypeKind::Void:
            return nullptr;
        case TypeKind::Integer: {
            auto* debug_type = di_builder->createBasicType(
                type->name, type->bit_width,
                type->is_signed ? llvm::dwarf::DW_ATE_signed
                                : llvm::dwarf::DW_ATE_unsigned);
            debug_types.emplace(type, debug_type);
            return debug_type;
        }
        case TypeKind::Float: {
            auto* debug_type = di_builder->createBasicType(
                type->name, type->bit_width, llvm::dwarf::DW_ATE_float);
            debug_types.emplace(type, debug_type);
            return debug_type;
        }
        case TypeKind::Char: {
            auto* debug_type = di_builder->createBasicType(
                "char", 8, llvm::dwarf::DW_ATE_unsigned_char);
            debug_types.emplace(type, debug_type);
            return debug_type;
        }
        case TypeKind::Bool: {
            auto* debug_type = di_builder->createBasicType(
                "bool", 1, llvm::dwarf::DW_ATE_boolean);
            debug_types.emplace(type, debug_type);
            return debug_type;
        }
        case TypeKind::Borrow:
        case TypeKind::Pointer: {
            auto* pointee = getDebugType(type->element_type);
            if (pointee == nullptr) {
                pointee = di_builder->createUnspecifiedType("void");
            }
            auto* debug_type = di_builder->createPointerType(
                pointee, data_layout.getPointerSizeInBits(0),
                static_cast<std::uint32_t>(
                    data_layout.getPointerABIAlignment(0).value() * 8));
            debug_types.emplace(type, debug_type);
            return debug_type;
        }
        case TypeKind::Array: {
            auto* element_type = getDebugType(type->element_type);
            std::array<llvm::Metadata*, 1> subscripts = {
                di_builder->getOrCreateSubrange(
                    0, static_cast<std::int64_t>(type->array_size))};
            auto* debug_type = di_builder->createArrayType(
                data_layout.getTypeAllocSizeInBits(lowerType(type)),
                static_cast<std::uint32_t>(
                    data_layout.getABITypeAlign(lowerType(type)).value() * 8),
                element_type, di_builder->getOrCreateArray(subscripts));
            debug_types.emplace(type, debug_type);
            return debug_type;
        }
        case TypeKind::Slice: {
            auto* file =
                compile_unit != nullptr ? compile_unit->getFile() : nullptr;
            auto* composite = di_builder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type, type->name, compile_unit,
                file, 0, 0, data_layout.getTypeAllocSizeInBits(lowerType(type)),
                static_cast<std::uint32_t>(
                    data_layout.getABITypeAlign(lowerType(type)).value() * 8),
                llvm::DINode::FlagZero, type->linkage_name);
            debug_types.emplace(type, composite);

            auto* pointer_type =
                getDebugType(types.getPointer(type->element_type));
            auto* length_type = getDebugType(types.i64Type());
            auto* layout = llvm::cast<llvm::StructType>(lowerType(type));
            const auto* struct_layout = data_layout.getStructLayout(layout);
            std::array<llvm::Metadata*, 2> elements = {
                di_builder->createMemberType(
                    composite, "data", file, 0,
                    data_layout.getTypeAllocSizeInBits(
                        layout->getElementType(0)),
                    static_cast<std::uint32_t>(
                        data_layout.getABITypeAlign(layout->getElementType(0))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(0),
                    llvm::DINode::FlagZero, pointer_type),
                di_builder->createMemberType(
                    composite, "len", file, 0,
                    data_layout.getTypeAllocSizeInBits(
                        layout->getElementType(1)),
                    static_cast<std::uint32_t>(
                        data_layout.getABITypeAlign(layout->getElementType(1))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(1),
                    llvm::DINode::FlagZero, length_type),
            };
            di_builder->replaceArrays(composite,
                                      di_builder->getOrCreateArray(elements));
            debug_types[type] = composite;
            return composite;
        }
        case TypeKind::Interface: {
            auto* file =
                compile_unit != nullptr ? compile_unit->getFile() : nullptr;
            auto* composite = di_builder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type, type->name, compile_unit,
                file, 0, 0, data_layout.getTypeAllocSizeInBits(lowerType(type)),
                static_cast<std::uint32_t>(
                    data_layout.getABITypeAlign(lowerType(type)).value() * 8),
                llvm::DINode::FlagZero, type->linkage_name);
            debug_types.emplace(type, composite);

            auto* opaque_pointer =
                getDebugType(types.getPointer(types.voidType()));
            auto* layout = llvm::cast<llvm::StructType>(lowerType(type));
            const auto* struct_layout = data_layout.getStructLayout(layout);
            std::array<llvm::Metadata*, 2> elements = {
                di_builder->createMemberType(
                    composite, "data", file, 0,
                    data_layout.getTypeAllocSizeInBits(
                        layout->getElementType(0)),
                    static_cast<std::uint32_t>(
                        data_layout.getABITypeAlign(layout->getElementType(0))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(0),
                    llvm::DINode::FlagZero, opaque_pointer),
                di_builder->createMemberType(
                    composite, "vtable", file, 0,
                    data_layout.getTypeAllocSizeInBits(
                        layout->getElementType(1)),
                    static_cast<std::uint32_t>(
                        data_layout.getABITypeAlign(layout->getElementType(1))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(1),
                    llvm::DINode::FlagZero, opaque_pointer),
            };
            di_builder->replaceArrays(composite,
                                      di_builder->getOrCreateArray(elements));
            debug_types[type] = composite;
            return composite;
        }
        case TypeKind::Struct: {
            const auto& decl = *type->struct_decl;
            auto* file =
                decl.range.source != nullptr
                    ? getOrCreateDebugFile(*decl.range.source)
                    : (compile_unit != nullptr ? compile_unit->getFile()
                                               : nullptr);
            const auto line =
                decl.range.source != nullptr
                    ? static_cast<unsigned>(
                          decl.range.source->location(decl.name_range.begin)
                              .line)
                    : 0U;
            auto* composite = di_builder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type, decl.name, compile_unit,
                file, line, 0,
                data_layout.getTypeAllocSizeInBits(lowerType(type)),
                static_cast<std::uint32_t>(
                    data_layout.getABITypeAlign(lowerType(type)).value() * 8),
                llvm::DINode::FlagZero, type->linkage_name);
            debug_types.emplace(type, composite);

            const auto* struct_layout =
                data_layout.getStructLayout(struct_types.at(type));
            std::vector<llvm::Metadata*> members;
            members.reserve(decl.fields.size());
            for (std::size_t index = 0; index < decl.fields.size(); ++index) {
                const auto& field = decl.fields[index];
                const auto field_line =
                    field.range.source != nullptr
                        ? static_cast<unsigned>(
                              field.range.source
                                  ->location(field.name_range.begin)
                                  .line)
                        : line;
                members.push_back(di_builder->createMemberType(
                    composite, field.name, file, field_line,
                    data_layout.getTypeAllocSizeInBits(
                        lowerType(field.resolved_type)),
                    static_cast<std::uint32_t>(
                        data_layout
                            .getABITypeAlign(lowerType(field.resolved_type))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(
                        static_cast<unsigned>(index)),
                    llvm::DINode::FlagZero, getDebugType(field.resolved_type)));
            }
            di_builder->replaceArrays(composite,
                                      di_builder->getOrCreateArray(members));
            debug_types[type] = composite;
            return composite;
        }
        case TypeKind::Enum: {
            const auto& decl = *type->enum_decl;
            auto* file =
                decl.range.source != nullptr
                    ? getOrCreateDebugFile(*decl.range.source)
                    : (compile_unit != nullptr ? compile_unit->getFile()
                                               : nullptr);
            const auto line =
                decl.range.source != nullptr
                    ? static_cast<unsigned>(
                          decl.range.source->location(decl.name_range.begin)
                              .line)
                    : 0U;
            auto* composite = di_builder->createReplaceableCompositeType(
                llvm::dwarf::DW_TAG_structure_type, decl.name, compile_unit,
                file, line, 0,
                data_layout.getTypeAllocSizeInBits(lowerType(type)),
                static_cast<std::uint32_t>(
                    data_layout.getABITypeAlign(lowerType(type)).value() * 8),
                llvm::DINode::FlagZero, type->linkage_name);
            debug_types.emplace(type, composite);

            const auto& layout = enum_layouts.at(type);
            const auto* struct_layout =
                data_layout.getStructLayout(layout.type);
            std::vector<llvm::Metadata*> members;
            members.push_back(di_builder->createMemberType(
                composite, "tag", file, line,
                data_layout.getTypeAllocSizeInBits(
                    layout.type->getElementType(0)),
                static_cast<std::uint32_t>(
                    data_layout.getABITypeAlign(layout.type->getElementType(0))
                        .value() *
                    8),
                struct_layout->getElementOffsetInBits(0),
                llvm::DINode::FlagZero, getDebugType(types.i64Type())));
            if (layout.payload_padding != 0) {
                auto* padding_type = getDebugType(
                    types.getArray(types.charType(), layout.payload_padding));
                members.push_back(di_builder->createMemberType(
                    composite, "payload_padding", file, line,
                    data_layout.getTypeAllocSizeInBits(
                        layout.type->getElementType(1)),
                    static_cast<std::uint32_t>(
                        data_layout
                            .getABITypeAlign(layout.type->getElementType(1))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(1),
                    llvm::DINode::FlagZero, padding_type));
            }
            if (layout.payload_size != 0) {
                auto* payload_type = getDebugType(
                    types.getArray(types.charType(), layout.payload_size));
                members.push_back(di_builder->createMemberType(
                    composite, "payload", file, line,
                    data_layout.getTypeAllocSizeInBits(
                        layout.type->getElementType(2)),
                    static_cast<std::uint32_t>(
                        data_layout
                            .getABITypeAlign(layout.type->getElementType(2))
                            .value() *
                        8),
                    struct_layout->getElementOffsetInBits(2),
                    llvm::DINode::FlagZero, payload_type));
            }
            di_builder->replaceArrays(composite,
                                      di_builder->getOrCreateArray(members));
            debug_types[type] = composite;
            return composite;
        }
        }
        return nullptr;
    }

    auto createDebugSubroutineType(const ast::FunctionDecl& decl)
        -> llvm::DISubroutineType* {
        std::vector<llvm::Metadata*> parameter_types;
        parameter_types.reserve(decl.parameters.size() + 1);
        parameter_types.push_back(getDebugType(decl.resolved_return_type));
        for (const auto& parameter : decl.parameters) {
            parameter_types.push_back(getDebugType(parameter.resolved_type));
        }
        return di_builder->createSubroutineType(
            di_builder->getOrCreateTypeArray(parameter_types));
    }

    auto beginDebugFunction(ast::FunctionDecl& decl, llvm::Function* function)
        -> void {
        if (!debugInfoEnabled()) {
            return;
        }

        auto* file =
            decl.range.source != nullptr
                ? getOrCreateDebugFile(*decl.range.source)
                : (compile_unit != nullptr ? compile_unit->getFile() : nullptr);
        const auto location =
            decl.range.source != nullptr
                ? decl.range.source->location(decl.name_range.begin)
                : SourceLocation{};
        auto sp_flags = llvm::DISubprogram::SPFlagDefinition;
        if (optimizationEnabled()) {
            sp_flags |= llvm::DISubprogram::SPFlagOptimized;
        }
        auto* subprogram =
            di_builder->createFunction(file, decl.name, decl.linkage_name, file,
                                       static_cast<unsigned>(location.line),
                                       createDebugSubroutineType(decl),
                                       static_cast<unsigned>(location.line),
                                       llvm::DINode::FlagPrototyped, sp_flags);
        function->setSubprogram(subprogram);
        current_subprogram = subprogram;
        debug_scope_stack.clear();
        debug_locals.clear();
    }

    auto declareDebugParameter(const ast::Parameter& parameter,
                               unsigned argument_index, llvm::Value* storage)
        -> void {
        if (!debugInfoEnabled() || current_subprogram == nullptr ||
            parameter.range.source == nullptr) {
            return;
        }
        const auto location =
            parameter.range.source->location(parameter.name_range.begin);
        auto* variable = di_builder->createParameterVariable(
            current_subprogram, parameter.name, argument_index,
            getOrCreateDebugFile(*parameter.range.source),
            static_cast<unsigned>(location.line),
            getDebugType(parameter.resolved_type), true);
        debug_locals[parameter.local_id] = variable;
        const auto debug_location = makeDebugLocation(parameter.name_range);
        di_builder->insertDeclare(
            storage, variable, di_builder->createExpression(),
            debug_location.get(), builder.GetInsertPoint());
    }

    auto declareDebugLocal(std::string_view name, SourceRange range,
                           const Type* type, std::size_t local_id,
                           llvm::Value* storage) -> void {
        if (!debugInfoEnabled() || range.source == nullptr ||
            currentDebugScope() == nullptr) {
            return;
        }
        const auto location = range.source->location(range.begin);
        auto* variable = di_builder->createAutoVariable(
            currentDebugScope(), std::string(name),
            getOrCreateDebugFile(*range.source),
            static_cast<unsigned>(location.line), getDebugType(type), true);
        debug_locals[local_id] = variable;
        const auto debug_location = makeDebugLocation(range);
        di_builder->insertDeclare(
            storage, variable, di_builder->createExpression(),
            debug_location.get(), builder.GetInsertPoint());
    }

    auto applyOfastFunctionAttributes(llvm::Function& function) -> void {
        if (options.optimization_level != OptimizationLevel::Ofast) {
            return;
        }
        function.addFnAttr("unsafe-fp-math", "true");
        function.addFnAttr("no-infs-fp-math", "true");
        function.addFnAttr("no-nans-fp-math", "true");
        function.addFnAttr("no-signed-zeros-fp-math", "true");
        function.addFnAttr("approx-func-fp-math", "true");

        llvm::FastMathFlags fast_math_flags;
        fast_math_flags.setFast();
        builder.setFastMathFlags(fast_math_flags);
    }

    auto getOrCreateStringLiteral(std::string_view value)
        -> llvm::GlobalVariable* {
        const auto key = std::string(value);
        if (const auto it = string_literals.find(key);
            it != string_literals.end()) {
            return it->second;
        }

        auto* constant = llvm::ConstantDataArray::getString(context, key, true);
        auto* global = new llvm::GlobalVariable(
            module, constant->getType(), true,
            llvm::GlobalValue::PrivateLinkage, constant,
            "str.lit." + std::to_string(next_string_literal_id++));
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        global->setAlignment(llvm::Align(1));
        string_literals.emplace(key, global);
        return global;
    }

    auto declareStructs(ast::Package& package)
        -> std::expected<void, Diagnostic> {
        auto declare_decl = [&](ast::Decl& decl) {
            if (auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
                struct_decl != nullptr &&
                struct_decl->type_parameters.empty()) {
                struct_types[struct_decl->resolved_type] =
                    llvm::StructType::create(context,
                                             struct_decl->linkage_name);
                return;
            }
            if (auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
                enum_decl != nullptr && enum_decl->type_parameters.empty()) {
                enum_layouts[enum_decl->resolved_type].type =
                    llvm::StructType::create(context, enum_decl->linkage_name);
            }
        };
        for (const auto& package_module : package.modules) {
            for (auto& decl : package_module->declarations) {
                declare_decl(decl);
            }
        }
        for (auto& decl : package.instantiated_declarations) {
            declare_decl(*decl);
        }

        auto define_decl = [&](ast::Decl& decl) {
            if (auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
                struct_decl != nullptr &&
                struct_decl->type_parameters.empty()) {
                std::vector<llvm::Type*> field_types;
                field_types.reserve(struct_decl->fields.size());
                for (const auto& field : struct_decl->fields) {
                    field_types.push_back(lowerType(field.resolved_type));
                }
                struct_types[struct_decl->resolved_type]->setBody(field_types,
                                                                  false);
                return;
            }
            if (auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
                enum_decl != nullptr && enum_decl->type_parameters.empty()) {
                auto& layout = enum_layouts[enum_decl->resolved_type];
                const auto& data_layout = module.getDataLayout();
                std::uint64_t max_payload_size = 0;
                std::uint64_t max_payload_align = 1;
                for (const auto& variant : enum_decl->variants) {
                    layout.payload_types.push_back(variant.resolved_type);
                    if (variant.resolved_type == nullptr) {
                        continue;
                    }
                    auto* payload_type = lowerType(variant.resolved_type);
                    max_payload_size = std::max<std::uint64_t>(
                        max_payload_size,
                        data_layout.getTypeAllocSize(payload_type));
                    max_payload_align = std::max<std::uint64_t>(
                        max_payload_align,
                        data_layout.getABITypeAlign(payload_type).value());
                }
                layout.payload_size = max_payload_size;
                layout.payload_padding =
                    (max_payload_align - (8 % max_payload_align)) %
                    max_payload_align;
                std::vector<llvm::Type*> fields = {
                    llvm::Type::getInt64Ty(context),
                    llvm::ArrayType::get(llvm::Type::getInt8Ty(context),
                                         layout.payload_padding),
                    llvm::ArrayType::get(llvm::Type::getInt8Ty(context),
                                         layout.payload_size),
                };
                layout.type->setBody(fields, false);
            }
        };
        for (const auto& module_decl : package.modules) {
            for (auto& decl : module_decl->declarations) {
                define_decl(decl);
            }
        }
        for (auto& decl : package.instantiated_declarations) {
            define_decl(*decl);
        }
        return {};
    }

    auto declareFunctions(ast::Package& package)
        -> std::expected<void, Diagnostic> {
        auto declare_decl =
            [&](ast::Decl& decl) -> std::expected<void, Diagnostic> {
            if (auto* function_decl = std::get_if<ast::FunctionDecl>(&decl);
                function_decl != nullptr &&
                function_decl->type_parameters.empty() &&
                !function_decl->intrinsic_lowering.has_value() &&
                !function_decl->builtin_lowering.has_value()) {
                std::vector<llvm::Type*> parameter_types;
                parameter_types.reserve(function_decl->parameters.size());
                for (const auto& parameter : function_decl->parameters) {
                    parameter_types.push_back(
                        lowerType(parameter.resolved_type));
                }
                auto* function_type = llvm::FunctionType::get(
                    lowerType(function_decl->resolved_return_type),
                    parameter_types, false);
                if (auto* existing =
                        module.getFunction(function_decl->linkage_name);
                    existing != nullptr) {
                    if (existing->getFunctionType() != function_type) {
                        return std::unexpected(Diagnostic(
                            "conflicting declarations for function '" +
                                function_decl->name + "'",
                            function_decl->range));
                    }
                    function_map[function_decl] = existing;
                    return {};
                }
                function_map[function_decl] = llvm::Function::Create(
                    function_type, llvm::Function::ExternalLinkage,
                    function_decl->linkage_name, module);
            }
            return {};
        };
        for (const auto& module_decl : package.modules) {
            for (auto& decl : module_decl->declarations) {
                auto declared = declare_decl(decl);
                if (!declared) {
                    return std::unexpected(declared.error());
                }
            }
        }
        for (auto& decl : package.instantiated_declarations) {
            auto declared = declare_decl(*decl);
            if (!declared) {
                return std::unexpected(declared.error());
            }
        }
        return {};
    }

    auto emitFunctions(ast::Package& package)
        -> std::expected<void, Diagnostic> {
        auto emit_decl =
            [&](ast::Decl& decl) -> std::expected<void, Diagnostic> {
            if (auto* function_decl = std::get_if<ast::FunctionDecl>(&decl);
                function_decl != nullptr &&
                function_decl->type_parameters.empty() &&
                !function_decl->is_extern && function_decl->body != nullptr) {
                auto emitted = emitFunction(*function_decl);
                if (!emitted) {
                    return std::unexpected(emitted.error());
                }
            }
            return {};
        };
        for (const auto& module_decl : package.modules) {
            for (auto& decl : module_decl->declarations) {
                auto emitted = emit_decl(decl);
                if (!emitted) {
                    return std::unexpected(emitted.error());
                }
            }
        }
        for (auto& decl : package.instantiated_declarations) {
            auto emitted = emit_decl(*decl);
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
        }
        return {};
    }

    auto emitFunction(ast::FunctionDecl& decl)
        -> std::expected<void, Diagnostic> {
        auto* function = function_map.at(&decl);
        auto* entry = llvm::BasicBlock::Create(context, "entry", function);
        builder.SetInsertPoint(entry);
        builder.clearFastMathFlags();
        applyOfastFunctionAttributes(*function);
        beginDebugFunction(decl, function);

        frame.locals.clear();

        std::size_t index = 0;
        for (auto& argument : function->args()) {
            const auto& parameter = decl.parameters[index];
            argument.setName(parameter.name);
            auto* slot =
                createEntryAlloca(parameter.name, parameter.resolved_type);
            builder.CreateStore(&argument, slot);
            frame.locals.emplace(parameter.local_id,
                                 LocalSlot{.address = slot,
                                           .type = parameter.resolved_type,
                                           .initialized_flag = nullptr});
            declareDebugParameter(parameter, static_cast<unsigned>(index + 1),
                                  slot);
            ++index;
        }

        auto emitted_body = emitBlock(*decl.body);
        if (!emitted_body) {
            return std::unexpected(emitted_body.error());
        }

        if (builder.GetInsertBlock() != nullptr &&
            builder.GetInsertBlock()->getTerminator() == nullptr) {
            auto dropped = emitDropLocalIds(decl.exit_drop_local_ids);
            if (!dropped) {
                return std::unexpected(dropped.error());
            }
            if (types.unqualify(decl.resolved_return_type) ==
                types.voidType()) {
                builder.CreateRetVoid();
            } else {
                return std::unexpected(Diagnostic(
                    "missing return in non-void function", decl.range));
            }
        }

        builder.SetCurrentDebugLocation({});
        debug_scope_stack.clear();
        current_subprogram = nullptr;
        debug_locals.clear();
        return {};
    }

    auto emitBlock(ast::Block& block) -> std::expected<void, Diagnostic> {
        ScopedDebugScope debug_scope(*this, block.range);
        for (auto& statement : block.statements) {
            auto emitted = emitStmt(*statement);
            if (!emitted) {
                return std::unexpected(emitted.error());
            }
            if (builder.GetInsertBlock() == nullptr ||
                builder.GetInsertBlock()->getTerminator() != nullptr) {
                break;
            }
        }
        if (builder.GetInsertBlock() != nullptr &&
            builder.GetInsertBlock()->getTerminator() == nullptr) {
            auto dropped = emitDropLocalIds(block.exit_drop_local_ids);
            if (!dropped) {
                return std::unexpected(dropped.error());
            }
        }
        return {};
    }

    auto emitStmt(ast::Stmt& stmt) -> std::expected<void, Diagnostic> {
        ScopedDebugLocation debug_location(*this, stmt.range);
        return std::visit(
            Overloaded{
                [&](ast::VarDeclStmt& var_decl)
                    -> std::expected<void, Diagnostic> {
                    auto* slot = createEntryAlloca(
                        var_decl.name, var_decl.type->resolved_type);
                    frame.locals.emplace(
                        var_decl.local_id,
                        LocalSlot{.address = slot,
                                  .type = var_decl.type->resolved_type,
                                  .initialized_flag = nullptr});
                    declareDebugLocal(var_decl.name, var_decl.name_range,
                                      var_decl.type->resolved_type,
                                      var_decl.local_id, slot);
                    if (var_decl.initializer != nullptr) {
                        auto* value = emitExpr(*var_decl.initializer);
                        if (value == nullptr) {
                            return std::unexpected(
                                Diagnostic("failed to emit initializer",
                                           var_decl.initializer->range));
                        }
                        builder.CreateStore(value, slot);
                    }
                    return {};
                },
                [&](ast::ExprStmt& expr_stmt)
                    -> std::expected<void, Diagnostic> {
                    if (emitExpr(*expr_stmt.expr) == nullptr) {
                        return std::unexpected(
                            Diagnostic("failed to emit expression statement",
                                       expr_stmt.expr->range));
                    }
                    return {};
                },
                [&](ast::AssignStmt& assign)
                    -> std::expected<void, Diagnostic> {
                    auto* target = emitPlaceAddress(*assign.target);
                    auto* value = emitExpr(*assign.value);
                    if (target == nullptr || value == nullptr) {
                        return std::unexpected(Diagnostic(
                            "failed to emit assignment", assign.target->range));
                    }
                    if (assign.drop_old_value) {
                        auto dropped =
                            emitDropValue(target, assign.target->resolved_type);
                        if (!dropped) {
                            return std::unexpected(dropped.error());
                        }
                    }
                    builder.CreateStore(value, target);
                    return {};
                },
                [&](ast::UpdateStmt& update)
                    -> std::expected<void, Diagnostic> {
                    auto* address = emitPlaceAddress(*update.target);
                    if (address == nullptr ||
                        update.target->resolved_type == nullptr) {
                        std::string detail;
                        if (const auto* name = std::get_if<ast::NameExpr>(
                                &update.target->node);
                            name != nullptr) {
                            detail =
                                " for local '" + name->name +
                                "' (local_id=" + std::to_string(name->local_id);
                            if (update.target->resolved_place.has_value()) {
                                detail +=
                                    ", place_root=" +
                                    std::to_string(
                                        update.target->resolved_place->root_id);
                            }
                            detail += ")";
                        }
                        return std::unexpected(Diagnostic(
                            "failed to emit increment/decrement" + detail,
                            update.target->range));
                    }

                    const auto* target_type =
                        types.unqualify(update.target->resolved_type);
                    auto* current_value = builder.CreateLoad(
                        lowerType(target_type), address, "update.current");
                    llvm::Value* next_value = nullptr;
                    if (types.isFloat(target_type)) {
                        const auto delta = update.is_increment ? 1.0 : -1.0;
                        next_value = builder.CreateFAdd(
                            current_value,
                            llvm::ConstantFP::get(lowerType(target_type),
                                                  delta),
                            "update.next");
                    } else if (types.isInteger(target_type)) {
                        const auto delta = update.is_increment
                                               ? std::int64_t{1}
                                               : std::int64_t{-1};
                        next_value = builder.CreateAdd(
                            current_value, integerConstant(target_type, delta),
                            "update.next");
                    } else if (target_type->kind == TypeKind::Pointer) {
                        const auto delta = update.is_increment
                                               ? std::int64_t{1}
                                               : std::int64_t{-1};
                        next_value = builder.CreateGEP(
                            lowerType(target_type->element_type), current_value,
                            llvm::ConstantInt::getSigned(
                                llvm::Type::getInt64Ty(context), delta),
                            "update.next");
                    }

                    if (next_value == nullptr) {
                        return std::unexpected(
                            Diagnostic("failed to emit increment/decrement",
                                       update.target->range));
                    }

                    builder.CreateStore(next_value, address);
                    return {};
                },
                [&](ast::ReturnStmt& ret) -> std::expected<void, Diagnostic> {
                    if (ret.value == nullptr) {
                        auto dropped = emitDropLocalIds(ret.drop_local_ids);
                        if (!dropped) {
                            return std::unexpected(dropped.error());
                        }
                        builder.CreateRetVoid();
                        return {};
                    }
                    auto* value = emitExpr(*ret.value);
                    if (value == nullptr) {
                        return std::unexpected(Diagnostic(
                            "failed to emit return value", ret.value->range));
                    }
                    auto dropped = emitDropLocalIds(ret.drop_local_ids);
                    if (!dropped) {
                        return std::unexpected(dropped.error());
                    }
                    builder.CreateRet(value);
                    return {};
                },
                [&](ast::DropStmt& drop_stmt)
                    -> std::expected<void, Diagnostic> {
                    const Type* drop_type = drop_stmt.value->resolved_type;
                    const auto* drop_base = drop_type == nullptr
                                                ? nullptr
                                                : types.unqualify(drop_type);
                    const auto is_whole_local_drop =
                        drop_stmt.value->resolved_place.has_value() &&
                        !drop_stmt.value->resolved_place->is_external &&
                        drop_stmt.value->resolved_place->fields.empty();
                    llvm::Value* address = nullptr;
                    if (drop_base != nullptr &&
                        drop_base->kind == TypeKind::Borrow &&
                        !is_whole_local_drop) {
                        address = emitExpr(*drop_stmt.value);
                        drop_type = drop_base->element_type;
                    } else {
                        address = emitPlaceAddress(*drop_stmt.value);
                    }
                    if (address == nullptr) {
                        return std::unexpected(Diagnostic(
                            "failed to emit drop", drop_stmt.value->range));
                    }
                    auto dropped = emitDropValue(address, drop_type);
                    if (!dropped) {
                        return std::unexpected(dropped.error());
                    }
                    return {};
                },
                [&](ast::IfStmt& if_stmt) -> std::expected<void, Diagnostic> {
                    auto* condition = emitExpr(*if_stmt.condition);
                    if (condition == nullptr) {
                        return std::unexpected(
                            Diagnostic("failed to emit if condition",
                                       if_stmt.condition->range));
                    }
                    auto* function = builder.GetInsertBlock()->getParent();
                    auto* then_block =
                        llvm::BasicBlock::Create(context, "if.then", function);
                    auto* else_block =
                        llvm::BasicBlock::Create(context, "if.else", function);
                    auto* merge_block =
                        llvm::BasicBlock::Create(context, "if.end", function);

                    builder.CreateCondBr(condition, then_block, else_block);

                    builder.SetInsertPoint(then_block);
                    auto then_emitted = emitBlock(*if_stmt.then_block);
                    if (!then_emitted) {
                        return std::unexpected(then_emitted.error());
                    }
                    if (builder.GetInsertBlock() != nullptr &&
                        builder.GetInsertBlock()->getTerminator() == nullptr) {
                        builder.CreateBr(merge_block);
                    }

                    builder.SetInsertPoint(else_block);
                    if (if_stmt.else_block != nullptr) {
                        auto else_emitted = emitBlock(*if_stmt.else_block);
                        if (!else_emitted) {
                            return std::unexpected(else_emitted.error());
                        }
                    }
                    if (builder.GetInsertBlock() != nullptr &&
                        builder.GetInsertBlock()->getTerminator() == nullptr) {
                        builder.CreateBr(merge_block);
                    }

                    if (merge_block->hasNPredecessorsOrMore(1)) {
                        builder.SetInsertPoint(merge_block);
                    } else {
                        merge_block->eraseFromParent();
                        builder.ClearInsertionPoint();
                    }
                    return {};
                },
                [&](ast::WhileStmt& while_stmt) {
                    return emitWhileStmt(while_stmt);
                },
                [&](ast::ForStmt& for_stmt) { return emitForStmt(for_stmt); },
                [&](ast::BreakStmt& break_stmt)
                    -> std::expected<void, Diagnostic> {
                    if (break_targets.empty()) {
                        return std::unexpected(
                            Diagnostic("break used outside of loop"));
                    }
                    auto dropped = emitDropLocalIds(break_stmt.drop_local_ids);
                    if (!dropped) {
                        return std::unexpected(dropped.error());
                    }
                    builder.CreateBr(break_targets.back());
                    builder.ClearInsertionPoint();
                    return {};
                },
                [&](ast::ContinueStmt& continue_stmt)
                    -> std::expected<void, Diagnostic> {
                    if (continue_targets.empty()) {
                        return std::unexpected(
                            Diagnostic("continue used outside of loop"));
                    }
                    auto dropped =
                        emitDropLocalIds(continue_stmt.drop_local_ids);
                    if (!dropped) {
                        return std::unexpected(dropped.error());
                    }
                    builder.CreateBr(continue_targets.back());
                    builder.ClearInsertionPoint();
                    return {};
                },
                [&](ast::UncheckedStmt& unchecked_stmt) {
                    return emitBlock(*unchecked_stmt.body);
                },
                [&](ast::SwitchStmt& switch_stmt) {
                    return emitSwitchStmt(switch_stmt);
                },
                [&](ast::Block& block_stmt) { return emitBlock(block_stmt); },
            },
            stmt.node);
    }

    auto emitWhileStmt(ast::WhileStmt& while_stmt)
        -> std::expected<void, Diagnostic> {
        auto* function = builder.GetInsertBlock()->getParent();
        auto* cond_block =
            llvm::BasicBlock::Create(context, "while.cond", function);
        auto* body_block =
            llvm::BasicBlock::Create(context, "while.body", function);
        auto* end_block =
            llvm::BasicBlock::Create(context, "while.end", function);

        builder.CreateBr(cond_block);

        builder.SetInsertPoint(cond_block);
        auto* condition = emitExpr(*while_stmt.condition);
        if (condition == nullptr) {
            return std::unexpected(Diagnostic("failed to emit while condition",
                                              while_stmt.condition->range));
        }
        builder.CreateCondBr(condition, body_block, end_block);

        builder.SetInsertPoint(body_block);
        break_targets.push_back(end_block);
        continue_targets.push_back(cond_block);
        auto emitted_body = emitBlock(*while_stmt.body);
        continue_targets.pop_back();
        break_targets.pop_back();
        if (!emitted_body) {
            return std::unexpected(emitted_body.error());
        }
        if (builder.GetInsertBlock() != nullptr &&
            builder.GetInsertBlock()->getTerminator() == nullptr) {
            builder.CreateBr(cond_block);
        }

        builder.SetInsertPoint(end_block);
        return {};
    }

    auto emitForStmt(ast::ForStmt& for_stmt)
        -> std::expected<void, Diagnostic> {
        if (for_stmt.initializer != nullptr) {
            auto emitted_initializer = emitStmt(*for_stmt.initializer);
            if (!emitted_initializer) {
                return std::unexpected(emitted_initializer.error());
            }
        }

        if (builder.GetInsertBlock() == nullptr ||
            builder.GetInsertBlock()->getTerminator() != nullptr) {
            return {};
        }

        auto* function = builder.GetInsertBlock()->getParent();
        auto* cond_block =
            llvm::BasicBlock::Create(context, "for.cond", function);
        auto* body_block =
            llvm::BasicBlock::Create(context, "for.body", function);
        auto* step_block =
            for_stmt.step != nullptr
                ? llvm::BasicBlock::Create(context, "for.step", function)
                : nullptr;
        auto* end_block =
            llvm::BasicBlock::Create(context, "for.end", function);

        builder.CreateBr(cond_block);

        builder.SetInsertPoint(cond_block);
        if (for_stmt.condition != nullptr) {
            auto* condition = emitExpr(*for_stmt.condition);
            if (condition == nullptr) {
                return std::unexpected(Diagnostic(
                    "failed to emit for condition", for_stmt.condition->range));
            }
            builder.CreateCondBr(condition, body_block, end_block);
        } else {
            builder.CreateBr(body_block);
        }

        builder.SetInsertPoint(body_block);
        break_targets.push_back(end_block);
        continue_targets.push_back(step_block != nullptr ? step_block
                                                         : cond_block);
        auto emitted_body = emitBlock(*for_stmt.body);
        continue_targets.pop_back();
        break_targets.pop_back();
        if (!emitted_body) {
            return std::unexpected(emitted_body.error());
        }
        if (builder.GetInsertBlock() != nullptr &&
            builder.GetInsertBlock()->getTerminator() == nullptr) {
            builder.CreateBr(step_block != nullptr ? step_block : cond_block);
        }

        if (step_block != nullptr) {
            builder.SetInsertPoint(step_block);
            auto emitted_step = emitStmt(*for_stmt.step);
            if (!emitted_step) {
                return std::unexpected(emitted_step.error());
            }
            if (builder.GetInsertBlock() != nullptr &&
                builder.GetInsertBlock()->getTerminator() == nullptr) {
                builder.CreateBr(cond_block);
            }
        }

        if (!end_block->hasNPredecessorsOrMore(1)) {
            end_block->eraseFromParent();
            builder.ClearInsertionPoint();
            return {};
        }

        builder.SetInsertPoint(end_block);
        auto dropped = emitDropLocalIds(for_stmt.exit_drop_local_ids);
        if (!dropped) {
            return std::unexpected(dropped.error());
        }
        return {};
    }

    auto emitSwitchStmt(ast::SwitchStmt& switch_stmt)
        -> std::expected<void, Diagnostic> {
        auto* function = builder.GetInsertBlock()->getParent();
        auto* merge_block =
            llvm::BasicBlock::Create(context, "switch.end", function);

        auto enum_storage = getSwitchStorage(switch_stmt);
        if (!enum_storage.has_value()) {
            return std::unexpected(
                Diagnostic("failed to lower switch scrutinee",
                           switch_stmt.scrutinee->range));
        }

        auto* tag_ptr =
            builder.CreateStructGEP(enum_storage->layout->type,
                                    enum_storage->address, 0, "switch.tag.ptr");
        auto* tag_value = builder.CreateLoad(llvm::Type::getInt64Ty(context),
                                             tag_ptr, "switch.tag");

        llvm::BasicBlock* default_block = nullptr;
        const auto explicit_case_count = static_cast<unsigned>(
            std::count_if(switch_stmt.cases.begin(), switch_stmt.cases.end(),
                          [](const ast::SwitchCase& switch_case) {
                              return !switch_case.is_default;
                          }));
        auto* switch_inst =
            builder.CreateSwitch(tag_value, nullptr, explicit_case_count);

        std::vector<llvm::BasicBlock*> case_blocks;
        case_blocks.reserve(switch_stmt.cases.size());
        for (std::size_t index = 0; index < switch_stmt.cases.size(); ++index) {
            auto* case_block =
                llvm::BasicBlock::Create(context, "switch.case", function);
            case_blocks.push_back(case_block);
            if (switch_stmt.cases[index].is_default) {
                default_block = case_block;
            } else {
                switch_inst->addCase(
                    llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context),
                        switch_stmt.cases[index].variant_index),
                    case_block);
            }
        }
        if (default_block == nullptr) {
            default_block = llvm::BasicBlock::Create(
                context, "switch.unreachable", function);
        }
        switch_inst->setDefaultDest(default_block);

        for (std::size_t index = 0; index < switch_stmt.cases.size(); ++index) {
            auto& switch_case = switch_stmt.cases[index];
            builder.SetInsertPoint(case_blocks[index]);

            if (switch_case.binding_local_id != 0U &&
                switch_case.binding_type != nullptr) {
                auto* binding_slot = createEntryAlloca(
                    switch_case.binding_name.value_or("case_value"),
                    switch_case.binding_type);
                frame.locals[switch_case.binding_local_id] =
                    LocalSlot{binding_slot, switch_case.binding_type, nullptr};
                if (switch_stmt.match_kind == ast::MatchKind::Move) {
                    auto* payload_ptr = createPayloadPointer(
                        *enum_storage->layout, enum_storage->address,
                        switch_case.variant_index);
                    auto* payload_value =
                        builder.CreateLoad(lowerType(switch_case.binding_type),
                                           payload_ptr, "case.payload");
                    builder.CreateStore(payload_value, binding_slot);
                } else {
                    auto* payload_ptr = createPayloadPointer(
                        *enum_storage->layout, enum_storage->address,
                        switch_case.variant_index);
                    builder.CreateStore(payload_ptr, binding_slot);
                }
            }

            auto emitted_case = emitBlock(*switch_case.body);
            if (!emitted_case) {
                return std::unexpected(emitted_case.error());
            }
            if (builder.GetInsertBlock() != nullptr &&
                builder.GetInsertBlock()->getTerminator() == nullptr) {
                builder.CreateBr(merge_block);
            }
        }

        if (!std::ranges::any_of(
                switch_stmt.cases,
                [](const ast::SwitchCase& c) { return c.is_default; })) {
            builder.SetInsertPoint(default_block);
            builder.CreateUnreachable();
        }

        if (merge_block->hasNPredecessorsOrMore(1)) {
            builder.SetInsertPoint(merge_block);
        } else {
            merge_block->eraseFromParent();
            builder.ClearInsertionPoint();
        }
        return {};
    }

    struct SwitchStorage {
        llvm::Value* address = nullptr;
        EnumLayout* layout = nullptr;
    };

    auto getSwitchStorage(ast::SwitchStmt& switch_stmt)
        -> std::optional<SwitchStorage> {
        if (switch_stmt.enum_decl == nullptr) {
            return std::nullopt;
        }

        auto* layout = &enum_layouts.at(switch_stmt.enum_decl->resolved_type);
        if (switch_stmt.match_kind == ast::MatchKind::Move) {
            auto* storage = createEntryAlloca(
                "switch.value", switch_stmt.enum_decl->resolved_type);
            auto* value = emitExpr(*switch_stmt.scrutinee);
            if (value == nullptr) {
                return std::nullopt;
            }
            builder.CreateStore(value, storage);
            return SwitchStorage{storage, layout};
        }

        auto* address = emitExpr(*switch_stmt.scrutinee);
        if (address == nullptr) {
            return std::nullopt;
        }
        return SwitchStorage{address, layout};
    }

    auto createPayloadPointer(EnumLayout& layout, llvm::Value* enum_address,
                              std::uint32_t variant_index) -> llvm::Value* {
        const auto* payload_type = layout.payload_types[variant_index];
        auto* payload_storage = builder.CreateStructGEP(
            layout.type, enum_address, 2, "payload.storage");
        if (payload_type == nullptr) {
            return payload_storage;
        }
        return builder.CreateBitCast(
            payload_storage, llvm::PointerType::get(context, 0), "payload.ptr");
    }

    auto interfaceStorageType() -> llvm::StructType* {
        auto* pointer_type = llvm::PointerType::get(context, 0);
        return llvm::StructType::get(context, {pointer_type, pointer_type});
    }

    auto sliceStorageType() -> llvm::StructType* {
        auto* pointer_type = llvm::PointerType::get(context, 0);
        return llvm::StructType::get(
            context, {pointer_type, lowerType(types.i64Type())});
    }

    auto emitInterfaceVTable(ast::Expr& expr) -> llvm::Constant* {
        if (expr.resolved_type == nullptr ||
            expr.resolved_type->kind != TypeKind::Interface) {
            return nullptr;
        }

        std::ostringstream key;
        key << expr.resolved_type->linkage_name;
        for (const auto* impl : expr.interface_impls) {
            key << '|';
            key << (impl != nullptr ? impl->linkage_name
                                    : std::string("<null>"));
        }

        if (const auto it = interface_vtables.find(key.str());
            it != interface_vtables.end()) {
            return llvm::ConstantExpr::getBitCast(
                it->second, llvm::PointerType::get(context, 0));
        }

        auto* pointer_type = llvm::PointerType::get(context, 0);
        auto* vtable_type =
            llvm::ArrayType::get(pointer_type, expr.interface_impls.size());
        std::vector<llvm::Constant*> entries;
        entries.reserve(expr.interface_impls.size());
        for (const auto* impl : expr.interface_impls) {
            entries.push_back(
                impl != nullptr ? llvm::ConstantExpr::getBitCast(
                                      function_map.at(impl), pointer_type)
                                : llvm::ConstantPointerNull::get(pointer_type));
        }

        auto* global = new llvm::GlobalVariable(
            module, vtable_type, true, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantArray::get(vtable_type, entries),
            "__cyan_iface_vtable");
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        interface_vtables.emplace(key.str(), global);
        return llvm::ConstantExpr::getBitCast(
            global, llvm::PointerType::get(context, 0));
    }

    auto emitInterfaceCoercion(ast::Expr& expr) -> llvm::Value* {
        if (expr.interface_source_type == nullptr) {
            return nullptr;
        }

        const auto* source_type = types.unqualify(expr.interface_source_type);
        llvm::Value* data_pointer = nullptr;
        llvm::Value* vtable_pointer = nullptr;
        if (source_type->kind == TypeKind::Interface) {
            const auto* target_type = expr.resolved_type == nullptr
                                          ? nullptr
                                          : types.unqualify(expr.resolved_type);
            if (target_type == nullptr ||
                target_type->kind != TypeKind::Interface) {
                return nullptr;
            }

            const auto interface_vtable_slice_start =
                [](const Type* source_interface,
                   const Type* target_interface) -> std::optional<std::size_t> {
                if (source_interface == nullptr ||
                    target_interface == nullptr ||
                    source_interface->kind != TypeKind::Interface ||
                    target_interface->kind != TypeKind::Interface) {
                    return std::nullopt;
                }

                std::optional<std::size_t> start_slot;
                std::optional<std::size_t> previous_slot;
                for (const auto* member : target_interface->interface_members) {
                    if (member == nullptr) {
                        continue;
                    }
                    const auto source_it = std::ranges::find(
                        source_interface->interface_members, member);
                    if (source_it ==
                        source_interface->interface_members.end()) {
                        return std::nullopt;
                    }
                    const auto slot = static_cast<std::size_t>(
                        source_it -
                        source_interface->interface_members.begin());
                    if (!start_slot.has_value()) {
                        start_slot = slot;
                    }
                    if (previous_slot.has_value() &&
                        slot != *previous_slot + 1) {
                        return std::nullopt;
                    }
                    previous_slot = slot;
                }
                return start_slot.value_or(0);
            };

            const auto source_expr_type = expr.interface_source_type;
            expr.interface_source_type = nullptr;
            auto* source_value = emitExpr(expr);
            expr.interface_source_type = source_expr_type;
            if (source_value == nullptr) {
                return nullptr;
            }

            data_pointer =
                builder.CreateExtractValue(source_value, {0}, "iface.data");
            auto* source_vtable_pointer =
                builder.CreateExtractValue(source_value, {1}, "iface.vtable");
            const auto slice_start =
                interface_vtable_slice_start(source_type, target_type);
            if (!slice_start.has_value()) {
                return nullptr;
            }
            vtable_pointer = builder.CreateGEP(
                llvm::PointerType::get(context, 0), source_vtable_pointer,
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                                       *slice_start),
                "iface.vtable.slice");
        } else if (source_type->kind == TypeKind::Borrow) {
            if (const auto* name = std::get_if<ast::NameExpr>(&expr.node);
                name != nullptr) {
                auto local_id = name->local_id;
                if (const auto it = frame.locals.find(local_id);
                    it != frame.locals.end()) {
                    data_pointer = builder.CreateLoad(
                        lowerType(source_type), it->second.address,
                        name->name + ".iface.data");
                } else if (expr.resolved_place.has_value() &&
                           !expr.resolved_place->is_external &&
                           expr.resolved_place->fields.empty()) {
                    local_id = expr.resolved_place->root_id;
                    if (const auto root_it = frame.locals.find(local_id);
                        root_it != frame.locals.end()) {
                        data_pointer = builder.CreateLoad(
                            lowerType(source_type), root_it->second.address,
                            name->name + ".iface.data");
                    }
                }
            } else if (const auto* unary =
                           std::get_if<ast::UnaryExpr>(&expr.node);
                       unary != nullptr &&
                       (unary->op == ast::UnaryOp::Borrow ||
                        unary->op == ast::UnaryOp::BorrowMut)) {
                data_pointer = emitBorrowOperand(*unary->operand);
            }
        } else {
            data_pointer = emitPlaceAddress(expr);
        }

        if (data_pointer == nullptr) {
            return nullptr;
        }
        if (vtable_pointer == nullptr) {
            vtable_pointer = emitInterfaceVTable(expr);
        }
        if (vtable_pointer == nullptr) {
            return nullptr;
        }

        llvm::Value* fat_value = llvm::UndefValue::get(interfaceStorageType());
        fat_value = builder.CreateInsertValue(fat_value, data_pointer, {0},
                                              "iface.data");
        fat_value = builder.CreateInsertValue(fat_value, vtable_pointer, {1},
                                              "iface.vtable");
        return fat_value;
    }

    auto emitSliceCoercion(ast::Expr& expr) -> llvm::Value* {
        if (expr.slice_source_type == nullptr) {
            return nullptr;
        }

        const auto* source_type = expr.slice_source_type;
        const auto* source_base = types.unqualify(source_type);
        llvm::Value* data_pointer = nullptr;
        std::uint64_t length = 0;

        if (source_base->kind == TypeKind::Array) {
            length = source_base->array_size;
            if (const auto* literal =
                    std::get_if<ast::StringLiteralExpr>(&expr.node);
                literal != nullptr) {
                auto* global = getOrCreateStringLiteral(literal->value);
                auto* zero =
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
                data_pointer = builder.CreateInBoundsGEP(
                    global->getValueType(), global, {zero, zero}, "slice.data");
            } else if (expr.resolved_place.has_value()) {
                auto* array_address = emitPlaceAddress(expr);
                if (array_address != nullptr && source_base->array_size != 0) {
                    auto* zero = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context), 0);
                    data_pointer = builder.CreateInBoundsGEP(
                        lowerType(source_type), array_address, {zero, zero},
                        "slice.data");
                }
            }
        } else if (source_base->kind == TypeKind::Borrow &&
                   source_base->element_type != nullptr &&
                   types.unqualify(source_base->element_type)->kind ==
                       TypeKind::Array) {
            const auto* array_type = types.unqualify(source_base->element_type);
            length = array_type->array_size;
            llvm::Value* array_pointer = nullptr;
            if (const auto* name = std::get_if<ast::NameExpr>(&expr.node);
                name != nullptr) {
                auto local_id = name->local_id;
                if (const auto it = frame.locals.find(local_id);
                    it != frame.locals.end()) {
                    array_pointer = builder.CreateLoad(
                        lowerType(source_type), it->second.address,
                        name->name + ".slice.src");
                } else if (expr.resolved_place.has_value() &&
                           !expr.resolved_place->is_external &&
                           expr.resolved_place->fields.empty()) {
                    local_id = expr.resolved_place->root_id;
                    if (const auto root_it = frame.locals.find(local_id);
                        root_it != frame.locals.end()) {
                        array_pointer = builder.CreateLoad(
                            lowerType(source_type), root_it->second.address,
                            name->name + ".slice.src");
                    }
                }
            } else if (const auto* unary =
                           std::get_if<ast::UnaryExpr>(&expr.node);
                       unary != nullptr &&
                       (unary->op == ast::UnaryOp::Borrow ||
                        unary->op == ast::UnaryOp::BorrowMut)) {
                array_pointer = emitBorrowOperand(*unary->operand);
            }

            if (array_pointer != nullptr && array_type->array_size != 0) {
                auto* zero =
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
                data_pointer = builder.CreateInBoundsGEP(
                    lowerType(array_type), array_pointer, {zero, zero},
                    "slice.data");
            }
        }

        if (data_pointer == nullptr) {
            data_pointer = llvm::ConstantPointerNull::get(
                llvm::PointerType::get(context, 0));
        }

        llvm::Value* slice_value = llvm::UndefValue::get(sliceStorageType());
        slice_value = builder.CreateInsertValue(slice_value, data_pointer, {0},
                                                "slice.ptr");
        slice_value = builder.CreateInsertValue(
            slice_value, lengthConstant(length), {1}, "slice.len");
        return slice_value;
    }

    auto emitTrapIf(llvm::Value* condition, std::string_view label) -> void {
        auto* function = builder.GetInsertBlock()->getParent();
        auto* trap_block = llvm::BasicBlock::Create(
            context, std::string(label) + ".trap", function);
        auto* cont_block = llvm::BasicBlock::Create(
            context, std::string(label) + ".cont", function);
        builder.CreateCondBr(condition, trap_block, cont_block);

        builder.SetInsertPoint(trap_block);
        auto trap = module.getOrInsertFunction(
            "llvm.trap",
            llvm::FunctionType::get(llvm::Type::getVoidTy(context), false));
        builder.CreateCall(trap, {});
        builder.CreateUnreachable();

        builder.SetInsertPoint(cont_block);
    }

    auto finishIntrinsicLowering(
        const ast::IntrinsicLowering& lowering, llvm::Value* emitted_call,
        const std::vector<llvm::Value*>& wrapper_arguments) -> llvm::Value* {
        if (lowering.return_argument_index.has_value()) {
            if (*lowering.return_argument_index >= wrapper_arguments.size()) {
                return nullptr;
            }
            return wrapper_arguments[*lowering.return_argument_index];
        }
        return emitted_call;
    }

    auto parseLoweringArgIndex(std::string_view text)
        -> std::optional<std::size_t> {
        if (!text.starts_with("arg") || text.size() <= 3) {
            return std::nullopt;
        }
        std::size_t value = 0;
        for (std::size_t index = 3; index < text.size(); ++index) {
            const auto ch = text[index];
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::size_t>(
                                     ch - static_cast<unsigned char>('0'));
        }
        return value;
    }

    auto parseIntegerLiteralSpec(std::string_view spec)
        -> std::optional<std::pair<unsigned, llvm::APInt>> {
        const auto colon = spec.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            colon + 1 >= spec.size()) {
            return std::nullopt;
        }
        const auto type_name = spec.substr(0, colon);
        const auto value_text = spec.substr(colon + 1);
        unsigned bits = 0;
        bool is_signed = false;
        if (type_name == "i8") {
            bits = 8;
            is_signed = true;
        } else if (type_name == "u8") {
            bits = 8;
        } else if (type_name == "i16") {
            bits = 16;
            is_signed = true;
        } else if (type_name == "u16") {
            bits = 16;
        } else if (type_name == "i32") {
            bits = 32;
            is_signed = true;
        } else if (type_name == "u32") {
            bits = 32;
        } else if (type_name == "i64") {
            bits = 64;
            is_signed = true;
        } else if (type_name == "u64") {
            bits = 64;
        } else if (type_name == "i128") {
            bits = 128;
            is_signed = true;
        } else if (type_name == "u128") {
            bits = 128;
        } else {
            return std::nullopt;
        }

        std::size_t offset = 0;
        bool negative = false;
        if (!value_text.empty() && value_text.front() == '-') {
            negative = true;
            offset = 1;
        }
        if (offset >= value_text.size()) {
            return std::nullopt;
        }
        const auto digits = value_text.substr(offset);
        for (const auto ch : digits) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
        }
        const auto parse_width = std::max<unsigned>(
            bits + 1U, static_cast<unsigned>(digits.size() * 4 + 1));
        auto value = llvm::APInt(
            parse_width, llvm::StringRef(digits.data(), digits.size()), 10);
        if (negative) {
            value = -value;
            return std::pair<unsigned, llvm::APInt>{bits,
                                                    value.sextOrTrunc(bits)};
        }
        return std::pair<unsigned, llvm::APInt>{
            bits,
            is_signed ? value.sextOrTrunc(bits) : value.zextOrTrunc(bits)};
    }

    auto parseFloatLiteralSpec(std::string_view spec)
        -> std::optional<std::pair<llvm::Type*, double>> {
        const auto colon = spec.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            colon + 1 >= spec.size()) {
            return std::nullopt;
        }
        const auto type_name = spec.substr(0, colon);
        const auto value_text = spec.substr(colon + 1);
        llvm::Type* type = nullptr;
        if (type_name == "f32") {
            type = llvm::Type::getFloatTy(context);
        } else if (type_name == "f64") {
            type = llvm::Type::getDoubleTy(context);
        } else {
            return std::nullopt;
        }
        char* end = nullptr;
        const auto parsed = std::strtod(std::string(value_text).c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return std::nullopt;
        }
        return std::pair<llvm::Type*, double>{type, parsed};
    }

    auto
    emitLoweringValueSpec(std::string_view spec,
                          const std::vector<llvm::Value*>& wrapper_arguments)
        -> llvm::Value* {
        if (const auto argument_index = parseLoweringArgIndex(spec);
            argument_index.has_value()) {
            if (*argument_index >= wrapper_arguments.size()) {
                return nullptr;
            }
            return wrapper_arguments[*argument_index];
        }
        if (spec == "true" || spec == "false") {
            return llvm::ConstantInt::getBool(context, spec == "true");
        }
        if (const auto integer_literal = parseIntegerLiteralSpec(spec);
            integer_literal.has_value()) {
            return llvm::ConstantInt::get(context, integer_literal->second);
        }
        if (const auto float_literal = parseFloatLiteralSpec(spec);
            float_literal.has_value()) {
            return llvm::ConstantFP::get(float_literal->first,
                                         float_literal->second);
        }
        return nullptr;
    }

    auto intrinsicReturnType(const ast::CallExpr& call,
                             const ast::IntrinsicLowering& lowering,
                             const std::vector<llvm::Value*>& wrapper_arguments)
        -> llvm::Type* {
        const auto spec = lowering.intrinsic_return_spec.value_or("wrapper");
        if (spec == "wrapper") {
            return lowerType(call.function->resolved_return_type);
        }
        if (spec == "void") {
            return llvm::Type::getVoidTy(context);
        }
        if (const auto argument_index = parseLoweringArgIndex(spec);
            argument_index.has_value()) {
            if (*argument_index >= wrapper_arguments.size()) {
                return nullptr;
            }
            return wrapper_arguments[*argument_index]->getType();
        }
        if (spec == "i8" || spec == "u8") {
            return llvm::Type::getInt8Ty(context);
        }
        if (spec == "i16" || spec == "u16") {
            return llvm::Type::getInt16Ty(context);
        }
        if (spec == "i32" || spec == "u32") {
            return llvm::Type::getInt32Ty(context);
        }
        if (spec == "i64" || spec == "u64") {
            return llvm::Type::getInt64Ty(context);
        }
        if (spec == "i128" || spec == "u128") {
            return llvm::Type::getIntNTy(context, 128);
        }
        if (spec == "f32") {
            return llvm::Type::getFloatTy(context);
        }
        if (spec == "f64") {
            return llvm::Type::getDoubleTy(context);
        }
        return nullptr;
    }

    auto emitIntrinsicLowering(ast::CallExpr& call) -> llvm::Value* {
        if (call.function == nullptr ||
            !call.function->intrinsic_lowering.has_value()) {
            return nullptr;
        }
        const auto& lowering = *call.function->intrinsic_lowering;
        std::vector<llvm::Value*> wrapper_arguments;
        wrapper_arguments.reserve(call.arguments.size());
        for (auto& argument : call.arguments) {
            auto* emitted_argument = emitExpr(*argument);
            if (emitted_argument == nullptr) {
                return nullptr;
            }
            wrapper_arguments.push_back(emitted_argument);
        }

        if (lowering.constant_kind == ast::LoweringConstantKind::NullValue) {
            if (call.function->resolved_return_type == nullptr) {
                return nullptr;
            }
            auto* return_type = lowerType(call.function->resolved_return_type);
            if (return_type->isVoidTy()) {
                return nullptr;
            }
            return llvm::Constant::getNullValue(return_type);
        }

        std::vector<llvm::Value*> intrinsic_arguments = wrapper_arguments;
        for (const auto& override : lowering.argument_overrides) {
            if (override.index >= intrinsic_arguments.size()) {
                intrinsic_arguments.resize(override.index + 1, nullptr);
            }
            intrinsic_arguments[override.index] =
                emitLoweringValueSpec(override.value_spec, wrapper_arguments);
        }
        if (std::ranges::any_of(intrinsic_arguments, [](llvm::Value* argument) {
                return argument == nullptr;
            })) {
            return nullptr;
        }

        std::vector<llvm::Type*> intrinsic_argument_types;
        intrinsic_argument_types.reserve(intrinsic_arguments.size());
        for (auto* argument : intrinsic_arguments) {
            intrinsic_argument_types.push_back(argument->getType());
        }

        auto intrinsic_id =
            llvm::Intrinsic::lookupIntrinsicID(lowering.intrinsic_name);
        if (intrinsic_id == llvm::Intrinsic::not_intrinsic) {
            return nullptr;
        }
        auto* ret_type = intrinsicReturnType(call, lowering, wrapper_arguments);
        if (ret_type == nullptr) {
            return nullptr;
        }
        auto* callee = llvm::Intrinsic::getOrInsertDeclaration(
            &module, intrinsic_id, ret_type, intrinsic_argument_types);
        auto* call_inst = builder.CreateCall(callee, intrinsic_arguments);
        return finishIntrinsicLowering(lowering, call_inst, wrapper_arguments);
    }

    auto atomicOrderIndex(ast::BuiltinCallKind kind)
        -> std::optional<std::int64_t> {
        switch (kind) {
        case ast::BuiltinCallKind::AtomicRelaxedOrder:
            return 0;
        case ast::BuiltinCallKind::AtomicAcquireOrder:
            return 1;
        case ast::BuiltinCallKind::AtomicReleaseOrder:
            return 2;
        case ast::BuiltinCallKind::AtomicAcqRelOrder:
            return 3;
        case ast::BuiltinCallKind::AtomicSeqCstOrder:
            return 4;
        default:
            return std::nullopt;
        }
    }

    auto atomicOrdering(ast::BuiltinCallKind kind)
        -> std::optional<llvm::AtomicOrdering> {
        switch (kind) {
        case ast::BuiltinCallKind::AtomicRelaxedOrder:
            return llvm::AtomicOrdering::Monotonic;
        case ast::BuiltinCallKind::AtomicAcquireOrder:
            return llvm::AtomicOrdering::Acquire;
        case ast::BuiltinCallKind::AtomicReleaseOrder:
            return llvm::AtomicOrdering::Release;
        case ast::BuiltinCallKind::AtomicAcqRelOrder:
            return llvm::AtomicOrdering::AcquireRelease;
        case ast::BuiltinCallKind::AtomicSeqCstOrder:
            return llvm::AtomicOrdering::SequentiallyConsistent;
        default:
            return std::nullopt;
        }
    }

    auto atomicOrdering(ast::Expr& expr)
        -> std::optional<llvm::AtomicOrdering> {
        const auto* order_call = std::get_if<ast::CallExpr>(&expr.node);
        if (order_call == nullptr) {
            return std::nullopt;
        }
        return atomicOrdering(order_call->builtin_kind);
    }

    auto atomicAlign(const Type* type) -> llvm::Align {
        return module.getDataLayout().getABITypeAlign(lowerType(type));
    }

    auto emitAtomicBuiltin(ast::CallExpr& call) -> llvm::Value* {
        switch (call.builtin_kind) {
        case ast::BuiltinCallKind::AtomicRelaxedOrder:
        case ast::BuiltinCallKind::AtomicAcquireOrder:
        case ast::BuiltinCallKind::AtomicReleaseOrder:
        case ast::BuiltinCallKind::AtomicAcqRelOrder:
        case ast::BuiltinCallKind::AtomicSeqCstOrder: {
            const auto order_index = atomicOrderIndex(call.builtin_kind);
            return order_index.has_value()
                       ? integerConstant(types.i64Type(), *order_index)
                       : nullptr;
        }
        case ast::BuiltinCallKind::AtomicLoad: {
            if (call.arguments.size() != 2 ||
                call.arguments[0]->resolved_type == nullptr) {
                return nullptr;
            }
            const auto* pointer_type =
                types.unqualify(call.arguments[0]->resolved_type);
            auto order = atomicOrdering(*call.arguments[1]);
            auto* pointer = emitExpr(*call.arguments[0]);
            if (pointer == nullptr || !order.has_value() ||
                pointer_type->element_type == nullptr) {
                return nullptr;
            }
            auto* load = builder.CreateLoad(
                lowerType(pointer_type->element_type), pointer, "atomic.load");
            load->setAtomic(*order);
            load->setAlignment(atomicAlign(pointer_type->element_type));
            return load;
        }
        case ast::BuiltinCallKind::AtomicStore: {
            if (call.arguments.size() != 3 ||
                call.arguments[0]->resolved_type == nullptr) {
                return nullptr;
            }
            const auto* pointer_type =
                types.unqualify(call.arguments[0]->resolved_type);
            auto order = atomicOrdering(*call.arguments[2]);
            auto* pointer = emitExpr(*call.arguments[0]);
            auto* value = emitExpr(*call.arguments[1]);
            if (pointer == nullptr || value == nullptr || !order.has_value() ||
                pointer_type->element_type == nullptr) {
                return nullptr;
            }
            auto* store = builder.CreateStore(value, pointer);
            store->setAtomic(*order);
            store->setAlignment(atomicAlign(pointer_type->element_type));
            return store;
        }
        case ast::BuiltinCallKind::AtomicExchange:
        case ast::BuiltinCallKind::AtomicFetchAdd:
        case ast::BuiltinCallKind::AtomicFetchSub:
        case ast::BuiltinCallKind::AtomicFetchAnd:
        case ast::BuiltinCallKind::AtomicFetchOr:
        case ast::BuiltinCallKind::AtomicFetchXor: {
            if (call.arguments.size() != 3 ||
                call.arguments[0]->resolved_type == nullptr) {
                return nullptr;
            }
            const auto* pointer_type =
                types.unqualify(call.arguments[0]->resolved_type);
            auto order = atomicOrdering(*call.arguments[2]);
            auto* pointer = emitExpr(*call.arguments[0]);
            auto* value = emitExpr(*call.arguments[1]);
            if (pointer == nullptr || value == nullptr || !order.has_value() ||
                pointer_type->element_type == nullptr) {
                return nullptr;
            }
            llvm::AtomicRMWInst::BinOp op = llvm::AtomicRMWInst::Xchg;
            switch (call.builtin_kind) {
            case ast::BuiltinCallKind::AtomicExchange:
                op = llvm::AtomicRMWInst::Xchg;
                break;
            case ast::BuiltinCallKind::AtomicFetchAdd:
                op = llvm::AtomicRMWInst::Add;
                break;
            case ast::BuiltinCallKind::AtomicFetchSub:
                op = llvm::AtomicRMWInst::Sub;
                break;
            case ast::BuiltinCallKind::AtomicFetchAnd:
                op = llvm::AtomicRMWInst::And;
                break;
            case ast::BuiltinCallKind::AtomicFetchOr:
                op = llvm::AtomicRMWInst::Or;
                break;
            case ast::BuiltinCallKind::AtomicFetchXor:
                op = llvm::AtomicRMWInst::Xor;
                break;
            default:
                break;
            }
            return builder.CreateAtomicRMW(
                op, pointer, value,
                llvm::MaybeAlign(atomicAlign(pointer_type->element_type)),
                *order, llvm::SyncScope::System);
        }
        case ast::BuiltinCallKind::AtomicCompareExchange: {
            if (call.arguments.size() != 5 ||
                call.arguments[0]->resolved_type == nullptr) {
                return nullptr;
            }
            const auto* pointer_type =
                types.unqualify(call.arguments[0]->resolved_type);
            auto success_order = atomicOrdering(*call.arguments[3]);
            auto failure_order = atomicOrdering(*call.arguments[4]);
            auto* pointer = emitExpr(*call.arguments[0]);
            auto* expected = emitExpr(*call.arguments[1]);
            auto* desired = emitExpr(*call.arguments[2]);
            if (pointer == nullptr || expected == nullptr ||
                desired == nullptr || !success_order.has_value() ||
                !failure_order.has_value() ||
                pointer_type->element_type == nullptr) {
                return nullptr;
            }
            auto* cmp = builder.CreateAtomicCmpXchg(
                pointer, expected, desired,
                llvm::MaybeAlign(atomicAlign(pointer_type->element_type)),
                *success_order, *failure_order, llvm::SyncScope::System);
            cmp->setWeak(false);
            return builder.CreateExtractValue(cmp, {0}, "atomic.cmpxchg.old");
        }
        case ast::BuiltinCallKind::AtomicFence: {
            if (call.arguments.size() != 1) {
                return nullptr;
            }
            auto order = atomicOrdering(*call.arguments[0]);
            return order.has_value()
                       ? builder.CreateFence(*order, llvm::SyncScope::System)
                       : nullptr;
        }
        default:
            return nullptr;
        }
    }

    auto emitRawDataBuiltin(ast::CallExpr& call) -> llvm::Value* {
        if (call.arguments.size() != 1) {
            return nullptr;
        }
        auto* slice_value = emitExpr(*call.arguments.front());
        if (slice_value == nullptr) {
            return nullptr;
        }
        return builder.CreateExtractValue(slice_value, {0}, "raw.data");
    }

    auto emitSubsliceBuiltin(ast::CallExpr& call) -> llvm::Value* {
        if (call.arguments.size() != 3 ||
            call.arguments.front()->resolved_type == nullptr) {
            return nullptr;
        }

        const auto* source_type = call.arguments.front()->resolved_type;
        const auto* source_base = types.unqualify(source_type);
        const Type* element_type = nullptr;
        llvm::Value* data_pointer = nullptr;
        llvm::Value* source_length = nullptr;

        if (source_base->kind == TypeKind::Array) {
            element_type = source_base->element_type;
            source_length = lengthConstant(source_base->array_size);
            if (const auto* literal = std::get_if<ast::StringLiteralExpr>(
                    &call.arguments.front()->node);
                literal != nullptr) {
                auto* global = getOrCreateStringLiteral(literal->value);
                auto* zero =
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
                data_pointer =
                    builder.CreateInBoundsGEP(global->getValueType(), global,
                                              {zero, zero}, "subslice.data");
            } else if (call.arguments.front()->resolved_place.has_value()) {
                auto* array_address = emitPlaceAddress(*call.arguments.front());
                if (array_address != nullptr && source_base->array_size != 0) {
                    auto* zero = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context), 0);
                    data_pointer = builder.CreateInBoundsGEP(
                        lowerType(source_type), array_address, {zero, zero},
                        "subslice.data");
                }
            }
        } else if (source_base->kind == TypeKind::Borrow &&
                   source_base->element_type != nullptr &&
                   types.unqualify(source_base->element_type)->kind ==
                       TypeKind::Array) {
            const auto* array_type = types.unqualify(source_base->element_type);
            element_type = array_type->element_type;
            source_length = lengthConstant(array_type->array_size);
            auto* array_pointer = emitExpr(*call.arguments.front());
            if (array_pointer != nullptr && array_type->array_size != 0) {
                auto* zero =
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
                data_pointer = builder.CreateInBoundsGEP(
                    lowerType(array_type), array_pointer, {zero, zero},
                    "subslice.data");
            }
        } else if (source_base->kind == TypeKind::Slice) {
            element_type = source_base->element_type;
            auto* slice_value = emitExpr(*call.arguments.front());
            if (slice_value == nullptr) {
                return nullptr;
            }
            data_pointer =
                builder.CreateExtractValue(slice_value, {0}, "subslice.ptr");
            source_length = builder.CreateExtractValue(slice_value, {1},
                                                       "subslice.src.len");
        } else {
            return nullptr;
        }

        if (element_type == nullptr || source_length == nullptr) {
            return nullptr;
        }

        if (data_pointer == nullptr) {
            data_pointer = llvm::ConstantPointerNull::get(
                llvm::PointerType::get(context, 0));
        }

        auto* start = emitExpr(*call.arguments[1]);
        auto* count = emitExpr(*call.arguments[2]);
        if (start == nullptr || count == nullptr) {
            return nullptr;
        }
        start = extendOrTruncateInteger(start, call.arguments[1]->resolved_type,
                                        types.i64Type());
        count = extendOrTruncateInteger(count, call.arguments[2]->resolved_type,
                                        types.i64Type());

        auto* zero = lengthConstant(0);
        auto* start_too_big =
            builder.CreateICmpUGT(start, source_length, "subslice.start.oob");
        auto* remaining = builder.CreateSelect(
            start_too_big, zero,
            builder.CreateSub(source_length, start, "subslice.remaining"));
        auto* count_too_big =
            builder.CreateICmpUGT(count, remaining, "subslice.len.oob");
        auto* invalid =
            builder.CreateOr(start_too_big, count_too_big, "subslice.invalid");
        emitTrapIf(invalid, "subslice");

        auto* result_pointer = builder.CreateGEP(
            lowerType(element_type), data_pointer, start, "subslice.ptr");
        llvm::Value* slice_value = llvm::UndefValue::get(sliceStorageType());
        slice_value = builder.CreateInsertValue(slice_value, result_pointer,
                                                {0}, "subslice.out.ptr");
        slice_value = builder.CreateInsertValue(slice_value, count, {1},
                                                "subslice.out.len");
        return slice_value;
    }

    auto emitFunctionPointerBuiltin(ast::CallExpr& call) -> llvm::Value* {
        if (call.builtin_target_function == nullptr) {
            return nullptr;
        }
        const auto function_it = function_map.find(call.builtin_target_function);
        if (function_it == function_map.end()) {
            return nullptr;
        }
        return llvm::ConstantExpr::getBitCast(
            function_it->second, llvm::PointerType::get(context, 0));
    }

    auto emitExpr(ast::Expr& expr) -> llvm::Value* {
        ScopedDebugLocation debug_location(*this, expr.range);
        if (expr.resolved_type != nullptr &&
            expr.resolved_type->kind == TypeKind::Interface &&
            expr.interface_source_type != nullptr) {
            return emitInterfaceCoercion(expr);
        }
        if (expr.resolved_type != nullptr &&
            types.unqualify(expr.resolved_type)->kind == TypeKind::Slice &&
            expr.slice_source_type != nullptr) {
            return emitSliceCoercion(expr);
        }
        return std::visit(
            Overloaded{
                [&](ast::IntegerLiteralExpr& literal) -> llvm::Value* {
                    return integerConstant(expr.resolved_type, literal.text);
                },
                [&](ast::FloatLiteralExpr& literal) -> llvm::Value* {
                    return llvm::ConstantFP::get(lowerType(expr.resolved_type),
                                                 literal.value);
                },
                [&](ast::CharLiteralExpr& literal) -> llvm::Value* {
                    return llvm::ConstantInt::get(
                        llvm::Type::getInt8Ty(context),
                        static_cast<unsigned char>(literal.value));
                },
                [&](ast::BoolLiteralExpr& literal) -> llvm::Value* {
                    return llvm::ConstantInt::getBool(context, literal.value);
                },
                [&](ast::StringLiteralExpr& literal) -> llvm::Value* {
                    auto* global = getOrCreateStringLiteral(literal.value);
                    auto* zero = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context), 0);
                    if (expr.resolved_type != nullptr &&
                        expr.resolved_type->kind == TypeKind::Pointer) {
                        return builder.CreateInBoundsGEP(global->getValueType(),
                                                         global, {zero, zero},
                                                         "str.ptr");
                    }
                    return builder.CreateLoad(global->getValueType(), global,
                                              "str.array");
                },
                [&](ast::NameExpr& name) -> llvm::Value* {
                    auto local_id = name.local_id;
                    if (const auto it = frame.locals.find(local_id);
                        it != frame.locals.end()) {
                        return builder.CreateLoad(lowerType(expr.resolved_type),
                                                  it->second.address,
                                                  name.name);
                    }
                    if (expr.resolved_place.has_value() &&
                        !expr.resolved_place->is_external &&
                        expr.resolved_place->fields.empty()) {
                        local_id = expr.resolved_place->root_id;
                    }
                    const auto it = frame.locals.find(local_id);
                    if (it == frame.locals.end()) {
                        return nullptr;
                    }
                    return builder.CreateLoad(lowerType(expr.resolved_type),
                                              it->second.address, name.name);
                },
                [&](ast::UnaryExpr& unary) -> llvm::Value* {
                    switch (unary.op) {
                    case ast::UnaryOp::Negate:
                        if (types.isFloat(expr.resolved_type)) {
                            return builder.CreateFNeg(emitExpr(*unary.operand),
                                                      "fnegtmp");
                        }
                        return builder.CreateNeg(emitExpr(*unary.operand),
                                                 "negtmp");
                    case ast::UnaryOp::LogicalNot:
                        return builder.CreateNot(emitExpr(*unary.operand),
                                                 "nottmp");
                    case ast::UnaryOp::BitwiseNot:
                        return builder.CreateNot(emitExpr(*unary.operand),
                                                 "bitnottmp");
                    case ast::UnaryOp::Dereference:
                        return builder.CreateLoad(lowerType(expr.resolved_type),
                                                  emitPlaceAddress(expr),
                                                  "deref");
                    case ast::UnaryOp::Borrow:
                    case ast::UnaryOp::BorrowMut:
                        return emitBorrowOperand(*unary.operand);
                    case ast::UnaryOp::Move:
                        return emitExpr(*unary.operand);
                    }
                    return nullptr;
                },
                [&](ast::BinaryExpr& binary) -> llvm::Value* {
                    auto* lhs = emitExpr(*binary.lhs);
                    auto* rhs = emitExpr(*binary.rhs);
                    const auto* lhs_type =
                        types.unqualify(binary.lhs->resolved_type);
                    const auto* rhs_type =
                        types.unqualify(binary.rhs->resolved_type);
                    const auto is_float =
                        types.isFloat(lhs_type) && lhs_type == rhs_type;
                    const auto is_unsigned_integer =
                        types.isUnsignedInteger(lhs_type) &&
                        lhs_type == rhs_type;
                    const auto lhs_is_pointer =
                        lhs_type != nullptr &&
                        lhs_type->kind == TypeKind::Pointer;
                    const auto rhs_is_pointer =
                        rhs_type != nullptr &&
                        rhs_type->kind == TypeKind::Pointer;
                    switch (binary.op) {
                    case ast::BinaryOp::Add:
                        if (is_float) {
                            return builder.CreateFAdd(lhs, rhs, "faddtmp");
                        }
                        if (lhs_is_pointer) {
                            return builder.CreateGEP(
                                lowerType(lhs_type->element_type), lhs, rhs,
                                "ptraddtmp");
                        }
                        if (rhs_is_pointer) {
                            return builder.CreateGEP(
                                lowerType(rhs_type->element_type), rhs, lhs,
                                "ptraddtmp");
                        }
                        return builder.CreateAdd(lhs, rhs, "addtmp");
                    case ast::BinaryOp::Subtract:
                        if (is_float) {
                            return builder.CreateFSub(lhs, rhs, "fsubtmp");
                        }
                        if (lhs_is_pointer) {
                            auto* neg_rhs =
                                builder.CreateNeg(rhs, "ptrsub.off");
                            return builder.CreateGEP(
                                lowerType(lhs_type->element_type), lhs, neg_rhs,
                                "ptrsubtmp");
                        }
                        return builder.CreateSub(lhs, rhs, "subtmp");
                    case ast::BinaryOp::Multiply:
                        if (is_float) {
                            return builder.CreateFMul(lhs, rhs, "fmultmp");
                        }
                        return builder.CreateMul(lhs, rhs, "multmp");
                    case ast::BinaryOp::Divide:
                        if (is_float) {
                            return builder.CreateFDiv(lhs, rhs, "fdivtmp");
                        }
                        if (is_unsigned_integer) {
                            return builder.CreateUDiv(lhs, rhs, "divtmp");
                        }
                        return builder.CreateSDiv(lhs, rhs, "divtmp");
                    case ast::BinaryOp::Remainder:
                        if (is_float) {
                            return builder.CreateFRem(lhs, rhs, "fmodtmp");
                        }
                        if (is_unsigned_integer) {
                            return builder.CreateURem(lhs, rhs, "modtmp");
                        }
                        return builder.CreateSRem(lhs, rhs, "modtmp");
                    case ast::BinaryOp::ShiftLeft:
                        rhs = extendOrTruncateInteger(
                            rhs, binary.rhs->resolved_type,
                            binary.lhs->resolved_type);
                        return builder.CreateShl(lhs, rhs, "shltmp");
                    case ast::BinaryOp::ShiftRight:
                        rhs = extendOrTruncateInteger(
                            rhs, binary.rhs->resolved_type,
                            binary.lhs->resolved_type);
                        if (types.isUnsignedInteger(lhs_type)) {
                            return builder.CreateLShr(lhs, rhs, "lshrtmp");
                        }
                        return builder.CreateAShr(lhs, rhs, "ashrtmp");
                    case ast::BinaryOp::BitwiseAnd:
                        return builder.CreateAnd(lhs, rhs, "bitandtmp");
                    case ast::BinaryOp::BitwiseXor:
                        return builder.CreateXor(lhs, rhs, "bitxortmp");
                    case ast::BinaryOp::BitwiseOr:
                        return builder.CreateOr(lhs, rhs, "bitortmp");
                    case ast::BinaryOp::Less:
                        if (is_float) {
                            return builder.CreateFCmpOLT(lhs, rhs, "fcmptmp");
                        }
                        if (is_unsigned_integer) {
                            return builder.CreateICmpULT(lhs, rhs, "cmptmp");
                        }
                        return builder.CreateICmpSLT(lhs, rhs, "cmptmp");
                    case ast::BinaryOp::LessEqual:
                        if (is_float) {
                            return builder.CreateFCmpOLE(lhs, rhs, "fcmptmp");
                        }
                        if (is_unsigned_integer) {
                            return builder.CreateICmpULE(lhs, rhs, "cmptmp");
                        }
                        return builder.CreateICmpSLE(lhs, rhs, "cmptmp");
                    case ast::BinaryOp::Greater:
                        if (is_float) {
                            return builder.CreateFCmpOGT(lhs, rhs, "fcmptmp");
                        }
                        if (is_unsigned_integer) {
                            return builder.CreateICmpUGT(lhs, rhs, "cmptmp");
                        }
                        return builder.CreateICmpSGT(lhs, rhs, "cmptmp");
                    case ast::BinaryOp::GreaterEqual:
                        if (is_float) {
                            return builder.CreateFCmpOGE(lhs, rhs, "fcmptmp");
                        }
                        if (is_unsigned_integer) {
                            return builder.CreateICmpUGE(lhs, rhs, "cmptmp");
                        }
                        return builder.CreateICmpSGE(lhs, rhs, "cmptmp");
                    case ast::BinaryOp::Equal:
                        if (is_float) {
                            return builder.CreateFCmpOEQ(lhs, rhs, "feqtmp");
                        }
                        return builder.CreateICmpEQ(lhs, rhs, "eqtmp");
                    case ast::BinaryOp::NotEqual:
                        if (is_float) {
                            return builder.CreateFCmpONE(lhs, rhs, "fnetmp");
                        }
                        return builder.CreateICmpNE(lhs, rhs, "netmp");
                    case ast::BinaryOp::LogicalAnd:
                        return builder.CreateAnd(lhs, rhs, "andtmp");
                    case ast::BinaryOp::LogicalOr:
                        return builder.CreateOr(lhs, rhs, "ortmp");
                    }
                    return nullptr;
                },
                [&](ast::CallExpr& call) -> llvm::Value* {
                    if (call.builtin_kind ==
                        ast::BuiltinCallKind::FunctionPointer) {
                        return emitFunctionPointerBuiltin(call);
                    }
                    if (call.builtin_kind != ast::BuiltinCallKind::None &&
                        call.builtin_kind != ast::BuiltinCallKind::Len &&
                        call.builtin_kind != ast::BuiltinCallKind::Subslice &&
                        call.builtin_kind != ast::BuiltinCallKind::RawData) {
                        return emitAtomicBuiltin(call);
                    }
                    if (call.builtin_kind == ast::BuiltinCallKind::Len) {
                        const auto* argument_type = types.unqualify(
                            call.arguments.front()->resolved_type);
                        if (argument_type->kind == TypeKind::Array) {
                            return llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(context),
                                argument_type->array_size);
                        }
                        auto* slice_value = emitExpr(*call.arguments.front());
                        if (slice_value == nullptr) {
                            return nullptr;
                        }
                        return builder.CreateExtractValue(slice_value, {1},
                                                          "slice.len");
                    }
                    if (call.builtin_kind == ast::BuiltinCallKind::Subslice) {
                        return emitSubsliceBuiltin(call);
                    }
                    if (call.builtin_kind == ast::BuiltinCallKind::RawData) {
                        return emitRawDataBuiltin(call);
                    }
                    if (call.function != nullptr &&
                        call.function->intrinsic_lowering.has_value()) {
                        return emitIntrinsicLowering(call);
                    }

                    if (call.dispatched_interface != nullptr &&
                        call.function == nullptr) {
                        auto* receiver = emitExpr(*call.arguments.front());
                        if (receiver == nullptr) {
                            return nullptr;
                        }

                        auto* data_pointer = builder.CreateExtractValue(
                            receiver, {0}, "iface.call.data");
                        auto* vtable_pointer = builder.CreateExtractValue(
                            receiver, {1}, "iface.call.vtable");
                        auto* slot_pointer = builder.CreateGEP(
                            llvm::PointerType::get(context, 0), vtable_pointer,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(context),
                                call.dispatched_interface_slot),
                            "iface.call.slot.ptr");
                        auto* function_pointer = builder.CreateLoad(
                            llvm::PointerType::get(context, 0), slot_pointer,
                            "iface.call.fn");
                        std::vector<llvm::Type*> parameter_types;
                        parameter_types.push_back(
                            llvm::PointerType::get(context, 0));
                        for (std::size_t index = 1;
                             index < call.arguments.size(); ++index) {
                            parameter_types.push_back(lowerType(
                                call.dispatched_interface->parameters[index]
                                    .resolved_type));
                        }
                        auto* function_type = llvm::FunctionType::get(
                            lowerType(call.dispatched_interface
                                          ->resolved_return_type),
                            parameter_types, false);
                        auto* typed_callee = builder.CreateBitCast(
                            function_pointer,
                            llvm::PointerType::get(context, 0),
                            "iface.call.callee");
                        std::vector<llvm::Value*> arguments;
                        arguments.reserve(call.arguments.size());
                        arguments.push_back(data_pointer);
                        for (std::size_t index = 1;
                             index < call.arguments.size(); ++index) {
                            auto* argument = emitExpr(*call.arguments[index]);
                            if (argument == nullptr) {
                                return nullptr;
                            }
                            arguments.push_back(argument);
                        }
                        return builder.CreateCall(
                            function_type, typed_callee, arguments,
                            call.dispatched_interface->resolved_return_type ==
                                    types.voidType()
                                ? ""
                                : "ifacetmp");
                    }

                    if (call.enum_decl != nullptr) {
                        auto* enum_storage =
                            createEntryAlloca(call.enum_decl->name + ".tmp",
                                              call.enum_decl->resolved_type);
                        builder.CreateStore(
                            llvm::Constant::getNullValue(
                                lowerType(call.enum_decl->resolved_type)),
                            enum_storage);

                        auto* tag_ptr = builder.CreateStructGEP(
                            lowerType(call.enum_decl->resolved_type),
                            enum_storage, 0, "enum.tag.ptr");
                        builder.CreateStore(llvm::ConstantInt::get(
                                                llvm::Type::getInt64Ty(context),
                                                call.variant_index),
                                            tag_ptr);

                        const auto& variant =
                            call.enum_decl->variants[call.variant_index];
                        if (variant.resolved_type != nullptr) {
                            auto* payload_ptr = createPayloadPointer(
                                enum_layouts.at(call.enum_decl->resolved_type),
                                enum_storage, call.variant_index);
                            auto* payload_value =
                                emitExpr(*call.arguments.front());
                            if (payload_value == nullptr) {
                                return nullptr;
                            }
                            builder.CreateStore(payload_value, payload_ptr);
                        }

                        return builder.CreateLoad(
                            lowerType(call.enum_decl->resolved_type),
                            enum_storage, "enumtmp");
                    }

                    auto* callee = function_map.at(call.function);
                    std::vector<llvm::Value*> arguments;
                    arguments.reserve(call.arguments.size());
                    for (std::size_t index = 0; index < call.arguments.size();
                         ++index) {
                        auto& argument = *call.arguments[index];
                        const auto* parameter_type =
                            call.function->parameters[index].resolved_type;
                        if (parameter_type->kind == TypeKind::Borrow) {
                            if (argument.resolved_type != nullptr &&
                                types.unqualify(argument.resolved_type)->kind ==
                                    TypeKind::Borrow) {
                                auto* lowered_argument = emitExpr(argument);
                                if (lowered_argument == nullptr) {
                                    return nullptr;
                                }
                                arguments.push_back(lowered_argument);
                            } else {
                                auto* lowered_argument =
                                    emitPlaceAddress(argument);
                                if (lowered_argument == nullptr) {
                                    return nullptr;
                                }
                                arguments.push_back(lowered_argument);
                            }
                        } else {
                            auto* lowered_argument = emitExpr(argument);
                            if (lowered_argument == nullptr) {
                                return nullptr;
                            }
                            arguments.push_back(lowered_argument);
                        }
                    }
                    return builder.CreateCall(
                        callee, arguments,
                        types.unqualify(call.function->resolved_return_type) ==
                                types.voidType()
                            ? ""
                            : "calltmp");
                },
                [&](ast::MemberExpr&) -> llvm::Value* {
                    return builder.CreateLoad(lowerType(expr.resolved_type),
                                              emitPlaceAddress(expr), "field");
                },
                [&](ast::IndexExpr&) -> llvm::Value* {
                    const auto* base_type =
                        types.unqualify(std::get<ast::IndexExpr>(expr.node)
                                            .base->resolved_type);
                    if (base_type->kind == TypeKind::Slice) {
                        auto* element_address = emitPlaceAddress(expr);
                        if (element_address == nullptr) {
                            return nullptr;
                        }
                        return builder.CreateLoad(lowerType(expr.resolved_type),
                                                  element_address,
                                                  "slice.index");
                    }
                    return builder.CreateLoad(lowerType(expr.resolved_type),
                                              emitPlaceAddress(expr), "index");
                },
                [&](ast::InitListExpr& init_list) -> llvm::Value* {
                    auto* storage =
                        createEntryAlloca("init.tmp", expr.resolved_type);
                    for (std::size_t index = 0;
                         index < init_list.elements.size(); ++index) {
                        auto* element = emitExpr(*init_list.elements[index]);
                        if (element == nullptr) {
                            return nullptr;
                        }
                        auto* field_address = builder.CreateStructGEP(
                            lowerType(expr.resolved_type), storage,
                            static_cast<unsigned>(index), "init.field.addr");
                        builder.CreateStore(element, field_address);
                    }
                    return builder.CreateLoad(lowerType(expr.resolved_type),
                                              storage, "inittmp");
                },
                [&](ast::ArrayLiteralExpr& array_literal) -> llvm::Value* {
                    if (types.unqualify(expr.resolved_type)->kind ==
                        TypeKind::Slice) {
                        const auto* slice_type =
                            types.unqualify(expr.resolved_type);
                        const auto* storage_type =
                            types.getArray(slice_type->element_type,
                                           array_literal.elements.size());
                        llvm::Value* storage = nullptr;
                        LocalSlot* storage_slot = nullptr;
                        if (expr.slice_storage_local_id != 0) {
                            const auto local_id = expr.slice_storage_local_id;
                            if (auto local_it = frame.locals.find(local_id);
                                local_it != frame.locals.end()) {
                                storage_slot = &local_it->second;
                            } else {
                                auto inserted = frame.locals.emplace(
                                    local_id,
                                    LocalSlot{
                                        createEntryAlloca("slice.array.tmp",
                                                          storage_type),
                                        storage_type,
                                        createEntryFlagAlloca(
                                            "slice.array.tmp.init"),
                                    });
                                storage_slot = &inserted.first->second;
                            }
                            storage = storage_slot->address;
                            auto dropped = emitDropSlot(*storage_slot);
                            if (!dropped) {
                                return nullptr;
                            }
                        } else {
                            storage = createEntryAlloca("slice.array.tmp",
                                                        storage_type);
                        }
                        auto* zero = llvm::ConstantInt::get(
                            llvm::Type::getInt64Ty(context), 0);
                        for (std::size_t index = 0;
                             index < array_literal.elements.size(); ++index) {
                            auto* element =
                                emitExpr(*array_literal.elements[index]);
                            if (element == nullptr) {
                                return nullptr;
                            }
                            auto* element_address = builder.CreateInBoundsGEP(
                                lowerType(storage_type), storage,
                                {zero,
                                 llvm::ConstantInt::get(
                                     llvm::Type::getInt64Ty(context), index)},
                                "slice.array.elem.addr");
                            builder.CreateStore(element, element_address);
                        }
                        if (storage_slot != nullptr &&
                            storage_slot->initialized_flag != nullptr) {
                            builder.CreateStore(
                                llvm::ConstantInt::getTrue(context),
                                storage_slot->initialized_flag);
                        }
                        llvm::Value* data_pointer =
                            llvm::ConstantPointerNull::get(
                                llvm::PointerType::get(context, 0));
                        if (!array_literal.elements.empty()) {
                            data_pointer = builder.CreateInBoundsGEP(
                                lowerType(storage_type), storage, {zero, zero},
                                "slice.data");
                        }
                        llvm::Value* slice_value =
                            llvm::UndefValue::get(sliceStorageType());
                        slice_value = builder.CreateInsertValue(
                            slice_value, data_pointer, {0}, "slice.ptr");
                        slice_value = builder.CreateInsertValue(
                            slice_value,
                            llvm::ConstantInt::get(
                                llvm::Type::getInt64Ty(context),
                                array_literal.elements.size()),
                            {1}, "slice.len");
                        return slice_value;
                    }

                    auto* storage =
                        createEntryAlloca("array.tmp", expr.resolved_type);
                    auto* zero = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context), 0);
                    for (std::size_t index = 0;
                         index < array_literal.elements.size(); ++index) {
                        auto* element =
                            emitExpr(*array_literal.elements[index]);
                        if (element == nullptr) {
                            return nullptr;
                        }
                        auto* element_address = builder.CreateInBoundsGEP(
                            lowerType(expr.resolved_type), storage,
                            {zero, llvm::ConstantInt::get(
                                       llvm::Type::getInt64Ty(context), index)},
                            "array.elem.addr");
                        builder.CreateStore(element, element_address);
                    }
                    return builder.CreateLoad(lowerType(expr.resolved_type),
                                              storage, "arraytmp");
                },
                [&](ast::CastExpr& cast_expr) -> llvm::Value* {
                    auto* operand = emitExpr(*cast_expr.operand);
                    if (operand == nullptr || expr.resolved_type == nullptr ||
                        cast_expr.operand->resolved_type == nullptr) {
                        return nullptr;
                    }

                    const auto* source_type =
                        types.unqualify(cast_expr.operand->resolved_type);
                    const auto* target_type =
                        types.unqualify(expr.resolved_type);
                    if (source_type == target_type) {
                        return operand;
                    }
                    if (cast_expr.cast_kind == ast::CastKind::Const) {
                        return operand;
                    }
                    if (cast_expr.cast_kind == ast::CastKind::Pointer) {
                        return builder.CreateBitCast(
                            operand, lowerType(expr.resolved_type),
                            "ptrcasttmp");
                    }
                    if (cast_expr.cast_kind == ast::CastKind::OwnerBorrow) {
                        return operand;
                    }
                    if (types.isFloat(target_type)) {
                        if (types.isFloat(source_type)) {
                            return builder.CreateFPCast(
                                operand, lowerType(target_type), "fpcasttmp");
                        }
                        if (types.isInteger(source_type)) {
                            if (types.isUnsignedInteger(source_type)) {
                                return builder.CreateUIToFP(
                                    operand, lowerType(target_type),
                                    "uitofptmp");
                            }
                            return builder.CreateSIToFP(
                                operand, lowerType(target_type), "sitofptmp");
                        }
                        auto* widened = builder.CreateZExt(
                            operand, integerTypeFor(types.i64Type()),
                            "char.to.int");
                        return builder.CreateUIToFP(
                            widened, lowerType(target_type), "charfptmp");
                    }
                    if (types.isInteger(target_type)) {
                        if (types.isFloat(source_type)) {
                            if (types.isUnsignedInteger(target_type)) {
                                return builder.CreateFPToUI(
                                    operand, lowerType(target_type),
                                    "fptouitmp");
                            }
                            return builder.CreateFPToSI(
                                operand, lowerType(target_type), "fptositmp");
                        }
                        if (types.isInteger(source_type)) {
                            return extendOrTruncateInteger(operand, source_type,
                                                           target_type);
                        }
                        return extendOrTruncateInteger(
                            operand, types.charType(), target_type);
                    }
                    if (target_type == types.charType()) {
                        if (types.isFloat(source_type)) {
                            return builder.CreateFPToUI(
                                operand, lowerType(target_type), "fptochartmp");
                        }
                        if (types.isInteger(source_type)) {
                            return extendOrTruncateInteger(operand, source_type,
                                                           target_type);
                        }
                        return operand;
                    }
                    return nullptr;
                },
                [&](ast::SizeofExpr& sizeof_expr) -> llvm::Value* {
                    return lengthConstant(
                        module.getDataLayout().getTypeAllocSize(
                            lowerType(sizeof_expr.operand_type)));
                },
            },
            expr.node);
    }

    auto emitPlaceAddress(ast::Expr& expr) -> llvm::Value* {
        ScopedDebugLocation debug_location(*this, expr.range);
        auto* address = std::visit(
            Overloaded{
                [&](ast::NameExpr& name) -> llvm::Value* {
                    auto local_id = name.local_id;
                    if (const auto it = frame.locals.find(local_id);
                        it != frame.locals.end()) {
                        return it->second.address;
                    }
                    if (expr.resolved_place.has_value() &&
                        !expr.resolved_place->is_external &&
                        expr.resolved_place->fields.empty()) {
                        local_id = expr.resolved_place->root_id;
                    }
                    const auto it = frame.locals.find(local_id);
                    return it == frame.locals.end() ? nullptr
                                                    : it->second.address;
                },
                [&](ast::MemberExpr& member) -> llvm::Value* {
                    llvm::Value* base_address = nullptr;
                    const Type* base_type = member.base->resolved_type;
                    if (base_type != nullptr &&
                        base_type->kind == TypeKind::Borrow) {
                        base_address = emitExpr(*member.base);
                        base_type = base_type->element_type;
                    } else {
                        base_address = emitPlaceAddress(*member.base);
                    }
                    if (base_address == nullptr) {
                        return nullptr;
                    }
                    return builder.CreateStructGEP(
                        lowerType(base_type), base_address, member.field_index,
                        "field.addr");
                },
                [&](ast::IndexExpr& index) -> llvm::Value* {
                    if (index.base->resolved_type == nullptr) {
                        return nullptr;
                    }
                    auto* offset = emitExpr(*index.index);
                    if (offset == nullptr) {
                        return nullptr;
                    }
                    auto* zero = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context), 0);
                    const auto* base_type =
                        types.unqualify(index.base->resolved_type);
                    if (base_type->kind == TypeKind::Pointer) {
                        auto* base = emitExpr(*index.base);
                        if (base == nullptr) {
                            return nullptr;
                        }
                        return builder.CreateGEP(
                            lowerType(base_type->element_type), base, offset,
                            "index.addr");
                    }
                    if (base_type->kind == TypeKind::Array) {
                        auto* base_address = emitPlaceAddress(*index.base);
                        if (base_address == nullptr) {
                            return nullptr;
                        }
                        return builder.CreateInBoundsGEP(
                            lowerType(base_type), base_address, {zero, offset},
                            "array.index.addr");
                    }
                    if (base_type->kind == TypeKind::Borrow &&
                        base_type->element_type != nullptr &&
                        base_type->element_type->kind == TypeKind::Array) {
                        auto* base_address = emitExpr(*index.base);
                        if (base_address == nullptr) {
                            return nullptr;
                        }
                        return builder.CreateInBoundsGEP(
                            lowerType(base_type->element_type), base_address,
                            {zero, offset}, "borrow.index.addr");
                    }
                    if (base_type->kind == TypeKind::Slice) {
                        auto* base = emitExpr(*index.base);
                        if (base == nullptr) {
                            return nullptr;
                        }
                        auto* data_pointer = builder.CreateExtractValue(
                            base, {0}, "slice.index.ptr");
                        return builder.CreateGEP(
                            lowerType(base_type->element_type), data_pointer,
                            offset, "slice.index.addr");
                    }
                    return nullptr;
                },
                [&](ast::UnaryExpr& unary) -> llvm::Value* {
                    if (unary.op != ast::UnaryOp::Dereference) {
                        return nullptr;
                    }
                    return emitExpr(*unary.operand);
                },
                [&](ast::CastExpr& cast_expr) -> llvm::Value* {
                    if (cast_expr.cast_kind != ast::CastKind::OwnerBorrow) {
                        return nullptr;
                    }
                    return emitExpr(*cast_expr.operand);
                },
                [&](auto&) -> llvm::Value* { return nullptr; },
            },
            expr.node);
        if (address != nullptr) {
            return address;
        }
        if (expr.resolved_place.has_value() &&
            expr.resolved_place->fields.empty()) {
            if (const auto it = frame.locals.find(expr.resolved_place->root_id);
                it != frame.locals.end()) {
                if (expr.resolved_type != nullptr &&
                    expr.resolved_type->kind == TypeKind::Borrow) {
                    return builder.CreateLoad(lowerType(expr.resolved_type),
                                              it->second.address,
                                              "borrow.place.addr");
                }
                return it->second.address;
            }
        }
        return nullptr;
    }

    auto emitBorrowOperand(ast::Expr& operand) -> llvm::Value* {
        if (operand.resolved_type != nullptr &&
            types.unqualify(operand.resolved_type)->kind == TypeKind::Borrow) {
            return emitExpr(operand);
        }
        return emitPlaceAddress(operand);
    }

    auto integerTypeFor(const Type* type) -> llvm::IntegerType* {
        return llvm::cast<llvm::IntegerType>(lowerType(type));
    }

    auto integerConstant(const Type* type, std::int64_t value)
        -> llvm::ConstantInt* {
        const auto* base = types.unqualify(type);
        const auto bit_width = base->kind == TypeKind::Integer
                                   ? base->bit_width
                                   : std::uint16_t{64};
        return llvm::ConstantInt::get(
            context,
            llvm::APInt(bit_width, static_cast<std::uint64_t>(value), true));
    }

    auto integerConstant(const Type* type, std::string_view text)
        -> llvm::ConstantInt* {
        const auto* base = types.unqualify(type);
        const auto bit_width = base->kind == TypeKind::Integer
                                   ? base->bit_width
                                   : std::uint16_t{64};
        auto value = llvm::APInt(bit_width,
                                 llvm::StringRef(text.data(), text.size()), 10);
        return llvm::ConstantInt::get(context, value);
    }

    auto lengthConstant(std::uint64_t value) -> llvm::ConstantInt* {
        return llvm::ConstantInt::get(integerTypeFor(types.i64Type()), value);
    }

    auto extendOrTruncateInteger(llvm::Value* value, const Type* source_type,
                                 const Type* target_type) -> llvm::Value* {
        const auto* source_base = types.unqualify(source_type);
        const auto* target_base = types.unqualify(target_type);
        if (source_base == target_base) {
            return value;
        }

        const auto source_width = source_base->kind == TypeKind::Char
                                      ? std::uint16_t{8}
                                      : source_base->bit_width;
        const auto target_width = target_base->kind == TypeKind::Char
                                      ? std::uint16_t{8}
                                      : target_base->bit_width;
        if (source_width == target_width) {
            return value;
        }
        if (source_width > target_width) {
            return builder.CreateTrunc(value, integerTypeFor(target_type),
                                       "int.trunc");
        }
        if (source_base->kind == TypeKind::Integer && source_base->is_signed) {
            return builder.CreateSExt(value, integerTypeFor(target_type),
                                      "int.sext");
        }
        return builder.CreateZExt(value, integerTypeFor(target_type),
                                  "int.zext");
    }

    auto lowerType(const Type* type) -> llvm::Type* {
        type = types.unqualify(type);
        switch (type->kind) {
        case TypeKind::Void:
            return llvm::Type::getVoidTy(context);
        case TypeKind::Integer:
            return llvm::Type::getIntNTy(context, type->bit_width);
        case TypeKind::Float:
            if (type->bit_width == 32) {
                return llvm::Type::getFloatTy(context);
            }
            return llvm::Type::getDoubleTy(context);
        case TypeKind::Char:
            return llvm::Type::getInt8Ty(context);
        case TypeKind::Bool:
            return llvm::Type::getInt1Ty(context);
        case TypeKind::Array:
            return llvm::ArrayType::get(lowerType(type->element_type),
                                        type->array_size);
        case TypeKind::Slice:
            return sliceStorageType();
        case TypeKind::Struct:
            return struct_types.at(type);
        case TypeKind::Enum:
            return enum_layouts.at(type).type;
        case TypeKind::Interface:
            return interfaceStorageType();
        case TypeKind::Borrow:
        case TypeKind::Pointer:
            return llvm::PointerType::get(context, 0);
        }
        return llvm::Type::getVoidTy(context);
    }

    auto createEntryAlloca(std::string_view name, const Type* type)
        -> llvm::AllocaInst* {
        llvm::IRBuilder<> entry_builder(
            &builder.GetInsertBlock()->getParent()->getEntryBlock(),
            builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
        return entry_builder.CreateAlloca(lowerType(type), nullptr, name);
    }

    auto createEntryFlagAlloca(std::string_view name) -> llvm::AllocaInst* {
        llvm::IRBuilder<> entry_builder(
            &builder.GetInsertBlock()->getParent()->getEntryBlock(),
            builder.GetInsertBlock()->getParent()->getEntryBlock().begin());
        auto* flag = entry_builder.CreateAlloca(llvm::Type::getInt1Ty(context),
                                                nullptr, name);
        entry_builder.CreateStore(llvm::ConstantInt::getFalse(context), flag);
        return flag;
    }

    auto emitDropSlot(LocalSlot& slot) -> std::expected<void, Diagnostic> {
        if (slot.initialized_flag == nullptr) {
            return emitDropValue(slot.address, slot.type);
        }

        auto* was_initialized = builder.CreateLoad(
            llvm::Type::getInt1Ty(context), slot.initialized_flag, "drop.init");
        auto* function = builder.GetInsertBlock()->getParent();
        auto* drop_block =
            llvm::BasicBlock::Create(context, "drop.slot", function);
        auto* cont_block =
            llvm::BasicBlock::Create(context, "drop.cont", function);
        builder.CreateCondBr(was_initialized, drop_block, cont_block);

        builder.SetInsertPoint(drop_block);
        auto dropped = emitDropValue(slot.address, slot.type);
        if (!dropped) {
            return std::unexpected(dropped.error());
        }
        builder.CreateStore(llvm::ConstantInt::getFalse(context),
                            slot.initialized_flag);
        builder.CreateBr(cont_block);

        builder.SetInsertPoint(cont_block);
        return {};
    }

    auto emitDropLocalIds(const std::vector<std::size_t>& drop_local_ids)
        -> std::expected<void, Diagnostic> {
        for (const auto local_id : drop_local_ids) {
            const auto local_it = frame.locals.find(local_id);
            if (local_it == frame.locals.end()) {
                return std::unexpected(
                    Diagnostic("missing local storage for drop"));
            }
            auto dropped = emitDropSlot(local_it->second);
            if (!dropped) {
                return std::unexpected(dropped.error());
            }
        }
        return {};
    }

    auto emitDropValue(llvm::Value* address, const Type* type)
        -> std::expected<void, Diagnostic> {
        type = types.unqualify(type);
        if (!types.needsDrop(type)) {
            return {};
        }

        if (const auto* drop_function = types.dropFunction(type);
            drop_function != nullptr) {
            const auto function_it = function_map.find(drop_function);
            if (function_it == function_map.end()) {
                return std::unexpected(
                    Diagnostic("missing drop hook declaration for type '" +
                               type->name + "'"));
            }
            builder.CreateCall(function_it->second, {address});
            return {};
        }

        switch (type->kind) {
        case TypeKind::Array: {
            auto* zero =
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0);
            for (std::uint64_t index = type->array_size; index > 0; --index) {
                auto* element_address = builder.CreateInBoundsGEP(
                    lowerType(type), address,
                    {zero, llvm::ConstantInt::get(
                               llvm::Type::getInt64Ty(context), index - 1)},
                    "drop.array.elem.addr");
                auto dropped =
                    emitDropValue(element_address, type->element_type);
                if (!dropped) {
                    return std::unexpected(dropped.error());
                }
            }
            return {};
        }
        case TypeKind::Struct: {
            const auto& fields = type->struct_decl->fields;
            for (std::size_t index = fields.size(); index > 0; --index) {
                const auto field_index = index - 1;
                auto* field_address = builder.CreateStructGEP(
                    lowerType(type), address,
                    static_cast<unsigned>(field_index), "drop.field.addr");
                auto dropped = emitDropValue(field_address,
                                             fields[field_index].resolved_type);
                if (!dropped) {
                    return std::unexpected(dropped.error());
                }
            }
            return {};
        }
        case TypeKind::Enum: {
            auto& layout = enum_layouts.at(type);
            auto* tag_pointer = builder.CreateStructGEP(layout.type, address, 0,
                                                        "drop.enum.tag.ptr");
            auto* tag_value = builder.CreateLoad(
                llvm::Type::getInt64Ty(context), tag_pointer, "drop.enum.tag");
            auto* function = builder.GetInsertBlock()->getParent();
            auto* merge_block =
                llvm::BasicBlock::Create(context, "drop.enum.end", function);
            auto* default_block = llvm::BasicBlock::Create(
                context, "drop.enum.unreachable", function);
            auto* switch_inst = builder.CreateSwitch(
                tag_value, default_block,
                static_cast<unsigned>(type->enum_decl->variants.size()));

            for (std::size_t index = 0;
                 index < type->enum_decl->variants.size(); ++index) {
                auto* case_block = llvm::BasicBlock::Create(
                    context, "drop.enum.case", function);
                switch_inst->addCase(
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context),
                                           index),
                    case_block);
                builder.SetInsertPoint(case_block);

                const auto& variant = type->enum_decl->variants[index];
                if (variant.resolved_type != nullptr) {
                    auto* payload_pointer = createPayloadPointer(
                        layout, address, static_cast<std::uint32_t>(index));
                    auto dropped =
                        emitDropValue(payload_pointer, variant.resolved_type);
                    if (!dropped) {
                        return std::unexpected(dropped.error());
                    }
                }

                if (builder.GetInsertBlock() != nullptr &&
                    builder.GetInsertBlock()->getTerminator() == nullptr) {
                    builder.CreateBr(merge_block);
                }
            }

            builder.SetInsertPoint(default_block);
            builder.CreateUnreachable();
            builder.SetInsertPoint(merge_block);
            return {};
        }
        case TypeKind::Void:
        case TypeKind::Integer:
        case TypeKind::Float:
        case TypeKind::Char:
        case TypeKind::Bool:
        case TypeKind::Interface:
        case TypeKind::Slice:
        case TypeKind::Borrow:
        case TypeKind::Pointer:
            return {};
        }
        return {};
    }

    TypeContext& types;
    CodegenOptions options;
    llvm::LLVMContext context;
    llvm::Module module;
    llvm::IRBuilder<> builder;
    std::unique_ptr<llvm::DIBuilder> di_builder;
    llvm::DICompileUnit* compile_unit = nullptr;
    llvm::DISubprogram* current_subprogram = nullptr;
    std::unordered_map<const Type*, llvm::StructType*> struct_types;
    std::unordered_map<const Type*, EnumLayout> enum_layouts;
    std::unordered_map<const ast::FunctionDecl*, llvm::Function*> function_map;
    std::unordered_map<std::string, llvm::GlobalVariable*> interface_vtables;
    std::unordered_map<const SourceFile*, llvm::DIFile*> debug_files;
    std::unordered_map<const Type*, llvm::DIType*> debug_types;
    std::unordered_map<std::size_t, llvm::DILocalVariable*> debug_locals;
    std::unordered_map<std::string, llvm::GlobalVariable*> string_literals;
    std::size_t next_string_literal_id = 0;
    std::vector<llvm::BasicBlock*> break_targets;
    std::vector<llvm::BasicBlock*> continue_targets;
    std::vector<llvm::DIScope*> debug_scope_stack;
    FunctionFrame frame;
};

} // namespace

CodeGenerator::CodeGenerator(TypeContext& types) : types(types) {}

auto CodeGenerator::emit(ast::Package& package,
                         const std::filesystem::path& output_path,
                         OutputKind output_kind, const CodegenOptions& options)
    -> std::expected<void, Diagnostic> {
    LLVMCodegen codegen(types, options);
    return codegen.emit(package, output_path, output_kind);
}

} // namespace cyan

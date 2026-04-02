#include "sema_detail.hpp"

namespace cyan {

using namespace detail;

namespace {

struct NormalizedImportBinding {
    ast::Module* imported_module = nullptr;
    std::optional<std::string> alias;
    SourceRange range;
    bool is_export = false;
    std::string key;
};

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

auto decl_range(const ast::Decl& decl) -> SourceRange {
    return std::visit([](const auto& inner) { return inner.range; }, decl);
}

auto normalize_import_bindings(const std::vector<ast::ImportDecl>& imports)
    -> std::vector<NormalizedImportBinding> {
    std::vector<NormalizedImportBinding> bindings;
    std::unordered_map<std::string, std::size_t> binding_indices;

    for (const auto& import_decl : imports) {
        if (import_decl.imported_module == nullptr) {
            continue;
        }

        std::string key = import_decl.imported_module->module_name;
        key += '|';
        key += import_decl.alias.has_value() ? *import_decl.alias : "*";

        if (const auto it = binding_indices.find(key);
            it != binding_indices.end()) {
            auto& binding = bindings[it->second];
            const auto was_exported = binding.is_export;
            binding.is_export = binding.is_export || import_decl.is_export;
            if (!was_exported && import_decl.is_export) {
                binding.range = import_decl.range;
            }
            continue;
        }

        binding_indices.emplace(key, bindings.size());
        bindings.push_back(NormalizedImportBinding{
            .imported_module = import_decl.imported_module,
            .alias = import_decl.alias,
            .range = import_decl.range,
            .is_export = import_decl.is_export,
            .key = std::move(key),
        });
    }

    std::ranges::sort(bindings, [](const auto& lhs, const auto& rhs) {
        return lhs.key < rhs.key;
    });
    return bindings;
}

auto split_qualified_name(std::string_view name) -> std::vector<std::string> {
    std::vector<std::string> parts;
    std::string current;
    for (const auto ch : name) {
        if (ch == '.') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

auto last_qualified_name_segment(std::string_view name) -> std::string_view {
    const auto separator = name.rfind('.');
    if (separator == std::string_view::npos) {
        return name;
    }
    return name.substr(separator + 1);
}

} // namespace

SemanticAnalyzer::SemanticAnalyzer(TypeContext& types) : types(types) {}

auto SemanticAnalyzer::exportScope(const ModuleScope& scope) const
    -> SemanticScope {
    SemanticScope exported;
    for (const auto& [name, module] : scope.import_namespaces) {
        exported.import_namespaces.emplace(name, module);
    }
    for (const auto& [name, decl] : scope.structs) {
        exported.structs.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.struct_templates) {
        exported.struct_templates.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.enums) {
        exported.enums.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.enum_templates) {
        exported.enum_templates.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.interfaces) {
        exported.interfaces.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.functions) {
        exported.functions.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.function_templates) {
        exported.function_templates.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.interface_impls) {
        exported.interface_impls.emplace(name, decl);
    }
    for (const auto& [name, decl] : scope.interface_impl_templates) {
        exported.interface_impl_templates.emplace(name, decl);
    }
    for (const auto& [name, variant] : scope.variants) {
        exported.variants.emplace(
            name, std::pair<const ast::EnumDecl*, std::size_t>{variant.first,
                                                               variant.second});
    }
    for (const auto& [name, variants] : scope.template_variants) {
        auto& exported_variants = exported.template_variants[name];
        exported_variants.reserve(variants.size());
        for (const auto& variant : variants) {
            exported_variants.emplace_back(variant.first, variant.second);
        }
    }
    return exported;
}

auto SemanticAnalyzer::recordLocalSymbol(ExportedSymbolMap& symbols,
                                         const ast::FunctionDecl& owner,
                                         const ast::VarDeclStmt& stmt,
                                         SourceRange declaration_range) const
    -> void {
    if (stmt.local_id == 0) {
        return;
    }

    symbols[stmt.local_id] = SemanticLocalSymbol{
        .kind = SemanticLocalSymbol::Kind::Var,
        .name = stmt.name,
        .declaration_range = declaration_range,
        .name_range = stmt.name_range,
        .type = stmt.type != nullptr ? stmt.type->resolved_type : nullptr,
        .owner_function = &owner,
        .var_decl = &stmt,
    };
}

auto SemanticAnalyzer::recordLocalSymbol(ExportedSymbolMap& symbols,
                                         const ast::FunctionDecl& owner,
                                         const ast::Parameter& parameter) const
    -> void {
    if (parameter.local_id == 0) {
        return;
    }

    symbols[parameter.local_id] = SemanticLocalSymbol{
        .kind = SemanticLocalSymbol::Kind::Parameter,
        .name = parameter.name,
        .declaration_range = parameter.range,
        .name_range = parameter.name_range,
        .type = parameter.resolved_type,
        .owner_function = &owner,
        .parameter = &parameter,
    };
}

auto SemanticAnalyzer::recordLocalSymbol(ExportedSymbolMap& symbols,
                                         const ast::FunctionDecl& owner,
                                         const ast::SwitchCase& switch_case,
                                         SourceRange declaration_range) const
    -> void {
    if (switch_case.binding_local_id == 0 ||
        !switch_case.binding_name.has_value() ||
        !switch_case.binding_name_range.has_value()) {
        return;
    }

    symbols[switch_case.binding_local_id] = SemanticLocalSymbol{
        .kind = SemanticLocalSymbol::Kind::SwitchBinding,
        .name = *switch_case.binding_name,
        .declaration_range = declaration_range,
        .name_range = *switch_case.binding_name_range,
        .type = switch_case.binding_type,
        .owner_function = &owner,
        .switch_case = &switch_case,
    };
}

auto SemanticAnalyzer::collectStmtLocalSymbols(ExportedSymbolMap& symbols,
                                               const ast::FunctionDecl& owner,
                                               const ast::Stmt& stmt) const
    -> void {
    std::visit(
        Overloaded{
            [&](const ast::VarDeclStmt& var_decl) {
                recordLocalSymbol(symbols, owner, var_decl, stmt.range);
            },
            [&](const ast::IfStmt& if_stmt) {
                collectBlockLocalSymbols(symbols, owner, *if_stmt.then_block);
                if (if_stmt.else_block != nullptr) {
                    collectBlockLocalSymbols(symbols, owner,
                                             *if_stmt.else_block);
                }
            },
            [&](const ast::WhileStmt& while_stmt) {
                collectBlockLocalSymbols(symbols, owner, *while_stmt.body);
            },
            [&](const ast::ForStmt& for_stmt) {
                if (for_stmt.initializer != nullptr) {
                    collectStmtLocalSymbols(symbols, owner,
                                            *for_stmt.initializer);
                }
                if (for_stmt.step != nullptr) {
                    collectStmtLocalSymbols(symbols, owner, *for_stmt.step);
                }
                collectBlockLocalSymbols(symbols, owner, *for_stmt.body);
            },
            [&](const ast::UncheckedStmt& unchecked_stmt) {
                collectBlockLocalSymbols(symbols, owner, *unchecked_stmt.body);
            },
            [&](const ast::SwitchStmt& switch_stmt) {
                for (const auto& switch_case : switch_stmt.cases) {
                    recordLocalSymbol(symbols, owner, switch_case,
                                      switch_case.range);
                    collectBlockLocalSymbols(symbols, owner, *switch_case.body);
                }
            },
            [&](const ast::Block& block) {
                collectBlockLocalSymbols(symbols, owner, block);
            },
            [&](const auto&) {},
        },
        stmt.node);
}

auto SemanticAnalyzer::collectBlockLocalSymbols(ExportedSymbolMap& symbols,
                                                const ast::FunctionDecl& owner,
                                                const ast::Block& block) const
    -> void {
    for (const auto& statement : block.statements) {
        collectStmtLocalSymbols(symbols, owner, *statement);
    }
}

auto SemanticAnalyzer::buildAnalysis() const -> SemanticAnalysis {
    SemanticAnalysis analysis;
    analysis.diagnostics = diagnostics;

    for (const auto& [module, scope] : local_scopes) {
        analysis.local_scopes.emplace(module, exportScope(scope));
    }
    for (const auto& [module, scope] : visible_scopes) {
        analysis.visible_scopes.emplace(module, exportScope(scope));
    }

    if (current_package == nullptr) {
        return analysis;
    }

    for (const auto& module : current_package->modules) {
        for (const auto& decl : module->declarations) {
            const auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr) {
                continue;
            }
            for (const auto& parameter : function->parameters) {
                recordLocalSymbol(analysis.local_symbols, *function, parameter);
            }
            if (function->body != nullptr) {
                collectBlockLocalSymbols(analysis.local_symbols, *function,
                                         *function->body);
            }
        }
    }

    return analysis;
}

auto SemanticAnalyzer::analyze(ast::Package& package) -> SemanticAnalysis {
    current_package = &package;
    active_module = nullptr;
    diagnostics.clear();
    local_scopes.clear();
    export_scopes.clear();
    visible_scopes.clear();
    building_export_scopes.clear();
    building_visible_scopes.clear();
    package_impls.clear();
    package_property_impls.clear();
    instantiated_structs.clear();
    instantiated_enums.clear();
    instantiated_functions.clear();
    next_external_root = 1;

    auto collected = collectDeclarations(package);
    static_cast<void>(collected);

    auto visible = buildVisibleScopes(package);
    static_cast<void>(visible);

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            if (auto* interface_decl = std::get_if<ast::InterfaceDecl>(&decl);
                interface_decl != nullptr) {
                auto analyzed = analyzeInterface(*interface_decl);
                static_cast<void>(analyzed);
            }
        }
    }

    auto registered_impls = registerImplDeclarations(package);
    if (!registered_impls) {
        report(registered_impls.error());
    }
    auto registered_properties = registerPropertyImplDeclarations(package);
    if (!registered_properties) {
        report(registered_properties.error());
    }

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            if (auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
                struct_decl != nullptr &&
                struct_decl->type_parameters.empty()) {
                auto analyzed = analyzeStruct(*struct_decl);
                static_cast<void>(analyzed);
                continue;
            }
            if (auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
                enum_decl != nullptr && enum_decl->type_parameters.empty()) {
                auto analyzed = analyzeEnum(*enum_decl);
                static_cast<void>(analyzed);
                continue;
            }
        }
    }

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr ||
                function->impl_target_kind == ast::ImplTargetKind::None) {
                continue;
            }

            ScopedModule scoped_module(active_module, function->owner_module);
            if (function->name == "drop") {
                if (function->return_type == nullptr) {
                    function->return_type =
                        make_type_syntax_from_type(types.voidType());
                }
                continue;
            }

            const auto* interface_decl = function->interface_decl;
            if (interface_decl == nullptr) {
                interface_decl = findVisibleInterface(function->name);
            }
            if (interface_decl == nullptr) {
                report(Diagnostic("unknown interface '" + function->name + "'",
                                  function->range));
                continue;
            }
            function->interface_decl = interface_decl;
            if (function->return_type == nullptr) {
                function->return_type =
                    clone_type_syntax(*interface_decl->return_type, {});
            }
        }
    }

    for (const auto& module : package.modules) {
        for (const auto& decl : module->declarations) {
            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr || !function->is_extern) {
                continue;
            }
            auto validated_extern = validateExternSignature(*function);
            if (!validated_extern) {
                report(validated_extern.error());
            }
        }
    }

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr || !function->type_parameters.empty()) {
                continue;
            }
            auto ensured_signature = ensureFunctionSignature(*function);
            if (!ensured_signature) {
                report(ensured_signature.error());
                continue;
            }
        }
    }

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr || !function->type_parameters.empty()) {
                continue;
            }
            if (function->resolved_return_type == nullptr) {
                continue;
            }
            if (function->body == nullptr && !function->is_extern) {
                continue;
            }
            if (std::ranges::any_of(
                    function->parameters, [](const auto& parameter) {
                        return parameter.resolved_type == nullptr;
                    })) {
                continue;
            }

            auto analyzed = analyzeFunction(*function);
            static_cast<void>(analyzed);
        }
    }

    return buildAnalysis();
}

auto SemanticAnalyzer::collectDeclarations(ast::Package& package)
    -> std::expected<void, Diagnostic> {
    for (const auto& module : package.modules) {
        auto& scope = local_scopes[module.get()];
        for (auto& decl : module->declarations) {
            if (auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
                struct_decl != nullptr) {
                struct_decl->owner_module = module.get();
                struct_decl->linkage_name =
                    use_qualified_linkage(package, *module)
                        ? make_decl_linkage_name(*module, struct_decl->name)
                        : struct_decl->name;
                if (struct_decl->type_parameters.empty()) {
                    struct_decl->resolved_type = types.registerStruct(
                        struct_decl->name, struct_decl->linkage_name,
                        struct_decl);
                }
                auto registered =
                    registerVisibleDecl(scope, decl, struct_decl->range);
                if (!registered) {
                    report(registered.error());
                }
                continue;
            }

            if (auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
                enum_decl != nullptr) {
                enum_decl->owner_module = module.get();
                enum_decl->linkage_name =
                    use_qualified_linkage(package, *module)
                        ? make_decl_linkage_name(*module, enum_decl->name)
                        : enum_decl->name;
                if (enum_decl->type_parameters.empty()) {
                    enum_decl->resolved_type = types.registerEnum(
                        enum_decl->name, enum_decl->linkage_name, enum_decl);
                }
                auto registered =
                    registerVisibleDecl(scope, decl, enum_decl->range);
                if (!registered) {
                    report(registered.error());
                }
                continue;
            }

            if (auto* interface_decl = std::get_if<ast::InterfaceDecl>(&decl);
                interface_decl != nullptr) {
                interface_decl->owner_module = module.get();
                auto registered =
                    registerVisibleDecl(scope, decl, interface_decl->range);
                if (!registered) {
                    report(registered.error());
                }
                continue;
            }

            if (auto* property_decl = std::get_if<ast::PropertyImplDecl>(&decl);
                property_decl != nullptr) {
                property_decl->owner_module = module.get();
                continue;
            }

            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr) {
                continue;
            }
            function->owner_module = module.get();
            if (function->impl_target_kind != ast::ImplTargetKind::None) {
                const auto receiver_suffix =
                    !function->parameters.empty() &&
                            function->parameters.front().type != nullptr &&
                            function->parameters.front().type->kind ==
                                ast::TypeSyntax::Kind::Borrow &&
                            function->parameters.front().type->element_type !=
                                nullptr
                        ? mangle_type_syntax(
                              *function->parameters.front().type->element_type)
                        : function->impl_target_name;
                const auto impl_name =
                    function->name == "drop"
                        ? "drop_" + receiver_suffix
                        : "impl_" + function->name + "_" + receiver_suffix;
                function->linkage_name =
                    !use_qualified_linkage(package, *module)
                        ? impl_name
                        : make_decl_linkage_name(*module, impl_name);
            } else {
                function->linkage_name =
                    function->is_extern ||
                            !use_qualified_linkage(package, *module)
                        ? function->name
                        : make_decl_linkage_name(*module, function->name);
            }
            auto registered = registerVisibleDecl(scope, decl, function->range);
            if (!registered) {
                report(registered.error());
            }
        }
    }

    return {};
}

auto SemanticAnalyzer::buildVisibleScopes(ast::Package& package)
    -> std::expected<void, Diagnostic> {
    for (const auto& module : package.modules) {
        for (const auto& import_decl : module->imports) {
            if (import_decl.imported_module == nullptr) {
                report(Diagnostic("unresolved import '" +
                                      describe_import_path(import_decl) + "'",
                                  import_decl.range));
            }
        }
    }

    for (const auto& module : package.modules) {
        auto visible_scope = buildVisibleScope(*module);
        if (!visible_scope) {
            report(visible_scope.error());
        }
    }
    return {};
}

auto SemanticAnalyzer::buildExportScope(const ast::Module& module)
    -> std::expected<const ModuleScope*, Diagnostic> {
    if (const auto it = export_scopes.find(&module);
        it != export_scopes.end()) {
        return &it->second;
    }
    if (building_export_scopes[&module]) {
        return std::unexpected(Diagnostic(
            "re-export cycle involving module '" + module.module_name + "'",
            module.source != nullptr ? module.source->range(0, 0)
                                     : SourceRange{}));
    }

    building_export_scopes[&module] = true;
    auto& scope = export_scopes[&module];

    for (auto& decl : module.declarations) {
        bool is_exported = false;
        std::visit(Overloaded{
                       [&](const ast::StructDecl& struct_decl) {
                           is_exported = struct_decl.is_export;
                       },
                       [&](const ast::EnumDecl& enum_decl) {
                           is_exported = enum_decl.is_export;
                       },
                       [&](const ast::InterfaceDecl& interface_decl) {
                           is_exported = interface_decl.is_export;
                       },
                       [&](const ast::FunctionDecl& function_decl) {
                           is_exported = function_decl.is_export;
                       },
                       [&](const ast::PropertyImplDecl&) {
                           is_exported = false;
                       },
                   },
                   decl);
        if (!is_exported) {
            continue;
        }
        auto& exported_decl = const_cast<ast::Decl&>(decl);
        auto registered =
            registerVisibleDecl(scope, exported_decl, decl_range(decl));
        if (!registered) {
            building_export_scopes[&module] = false;
            return std::unexpected(registered.error());
        }
    }

    for (const auto& binding : normalize_import_bindings(module.imports)) {
        if (!binding.is_export) {
            continue;
        }
        if (binding.imported_module == nullptr) {
            continue;
        }
        if (binding.alias.has_value()) {
            auto imported_scope = buildExportScope(*binding.imported_module);
            if (!imported_scope) {
                building_export_scopes[&module] = false;
                return std::unexpected(imported_scope.error());
            }
            auto registered = registerImportNamespace(
                scope, *binding.alias, binding.imported_module, binding.range);
            if (!registered) {
                building_export_scopes[&module] = false;
                return std::unexpected(registered.error());
            }
            continue;
        }
        if (building_export_scopes[binding.imported_module]) {
            building_export_scopes[&module] = false;
            return std::unexpected(
                Diagnostic("re-export cycle involving module '" +
                               binding.imported_module->module_name + "'",
                           binding.range));
        }
        auto imported_scope = buildExportScope(*binding.imported_module);
        if (!imported_scope) {
            building_export_scopes[&module] = false;
            return std::unexpected(imported_scope.error());
        }
        auto merged =
            mergeImportedScope(scope, **imported_scope, binding.range);
        if (!merged) {
            building_export_scopes[&module] = false;
            return std::unexpected(merged.error());
        }
    }

    building_export_scopes[&module] = false;
    return &scope;
}

auto SemanticAnalyzer::buildVisibleScope(const ast::Module& module)
    -> std::expected<const ModuleScope*, Diagnostic> {
    if (const auto it = visible_scopes.find(&module);
        it != visible_scopes.end()) {
        return &it->second;
    }
    if (building_visible_scopes[&module]) {
        return std::unexpected(Diagnostic(
            "visible scope cycle involving module '" + module.module_name + "'",
            module.source != nullptr ? module.source->range(0, 0)
                                     : SourceRange{}));
    }

    building_visible_scopes[&module] = true;
    auto scope = localScopeFor(module);

    for (const auto& binding : normalize_import_bindings(module.imports)) {
        if (binding.imported_module == nullptr) {
            continue;
        }
        if (binding.alias.has_value()) {
            auto imported_scope = buildExportScope(*binding.imported_module);
            if (!imported_scope) {
                building_visible_scopes[&module] = false;
                return std::unexpected(imported_scope.error());
            }
            auto registered = registerImportNamespace(
                scope, *binding.alias, binding.imported_module, binding.range);
            if (!registered) {
                building_visible_scopes[&module] = false;
                return std::unexpected(registered.error());
            }
            continue;
        }

        auto imported_scope = buildExportScope(*binding.imported_module);
        if (!imported_scope) {
            building_visible_scopes[&module] = false;
            return std::unexpected(imported_scope.error());
        }
        auto merged =
            mergeImportedScope(scope, **imported_scope, binding.range);
        if (!merged) {
            building_visible_scopes[&module] = false;
            return std::unexpected(merged.error());
        }
    }

    auto [it, _] = visible_scopes.emplace(&module, std::move(scope));
    building_visible_scopes[&module] = false;
    return &it->second;
}

auto SemanticAnalyzer::registerImplDeclarations(ast::Package& package)
    -> std::expected<void, Diagnostic> {
    package_impls.clear();

    struct PatternTerm {
        enum class Kind : std::uint8_t {
            Variable,
            Named,
            Borrow,
            Slice,
            Pointer,
            Array,
        };

        Kind kind = Kind::Named;
        std::string id;
        bool is_mut = false;
        std::uint64_t array_size = 0;
        std::shared_ptr<PatternTerm> element_type;
        std::vector<PatternTerm> type_arguments;
    };

    using Substitutions = std::unordered_map<std::string, PatternTerm>;

    std::function<std::expected<PatternTerm, Diagnostic>(
        const ast::TypeSyntax&, const ast::Module&,
        const std::vector<std::string>&, std::string_view)>
        build_pattern_term =
            [&](const ast::TypeSyntax& syntax, const ast::Module& owner_module,
                const std::vector<std::string>& type_parameters,
                std::string_view variable_prefix)
        -> std::expected<PatternTerm, Diagnostic> {
        if (syntax.kind == ast::TypeSyntax::Kind::Named &&
            syntax.type_arguments.empty() &&
            std::ranges::find(type_parameters, syntax.name) !=
                type_parameters.end()) {
            PatternTerm term;
            term.kind = PatternTerm::Kind::Variable;
            term.id = std::string(variable_prefix) + syntax.name;
            return term;
        }

        switch (syntax.kind) {
        case ast::TypeSyntax::Kind::Named: {
            PatternTerm term;
            term.kind = PatternTerm::Kind::Named;
            if (syntax.type_arguments.empty()) {
                if (const auto* named =
                        findNamedTypeInModule(owner_module, syntax.name);
                    named != nullptr) {
                    term.id = types.unqualify(named)->linkage_name;
                } else if (findStructTemplateInModule(owner_module,
                                                      syntax.name) != nullptr ||
                           findEnumTemplateInModule(owner_module,
                                                    syntax.name) != nullptr) {
                    return std::unexpected(
                        Diagnostic("generic type '" + syntax.name +
                                       "' requires explicit type arguments",
                                   syntax.range));
                } else {
                    return std::unexpected(Diagnostic(
                        "unknown type '" + syntax.name + "'", syntax.range));
                }
                return term;
            }

            if (const auto* struct_decl =
                    findStructTemplateInModule(owner_module, syntax.name);
                struct_decl != nullptr) {
                term.id = struct_decl->linkage_name;
            } else if (const auto* enum_decl =
                           findEnumTemplateInModule(owner_module, syntax.name);
                       enum_decl != nullptr) {
                term.id = enum_decl->linkage_name;
            } else if (findNamedTypeInModule(owner_module, syntax.name) !=
                       nullptr) {
                return std::unexpected(Diagnostic(
                    "type '" + syntax.name + "' is not generic", syntax.range));
            } else {
                return std::unexpected(Diagnostic(
                    "unknown type '" + syntax.name + "'", syntax.range));
            }

            term.type_arguments.reserve(syntax.type_arguments.size());
            for (const auto& type_argument : syntax.type_arguments) {
                auto built_argument =
                    build_pattern_term(*type_argument, owner_module,
                                       type_parameters, variable_prefix);
                if (!built_argument) {
                    return std::unexpected(built_argument.error());
                }
                term.type_arguments.push_back(std::move(*built_argument));
            }
            return term;
        }
        case ast::TypeSyntax::Kind::Borrow: {
            auto element =
                build_pattern_term(*syntax.element_type, owner_module,
                                   type_parameters, variable_prefix);
            if (!element) {
                return std::unexpected(element.error());
            }
            PatternTerm term;
            term.kind = PatternTerm::Kind::Borrow;
            term.is_mut = syntax.is_mut;
            term.element_type =
                std::make_shared<PatternTerm>(std::move(*element));
            return term;
        }
        case ast::TypeSyntax::Kind::Slice: {
            auto element =
                build_pattern_term(*syntax.element_type, owner_module,
                                   type_parameters, variable_prefix);
            if (!element) {
                return std::unexpected(element.error());
            }
            PatternTerm term;
            term.kind = PatternTerm::Kind::Slice;
            term.element_type =
                std::make_shared<PatternTerm>(std::move(*element));
            return term;
        }
        case ast::TypeSyntax::Kind::Pointer: {
            auto element =
                build_pattern_term(*syntax.element_type, owner_module,
                                   type_parameters, variable_prefix);
            if (!element) {
                return std::unexpected(element.error());
            }
            PatternTerm term;
            term.kind = PatternTerm::Kind::Pointer;
            term.element_type =
                std::make_shared<PatternTerm>(std::move(*element));
            return term;
        }
        case ast::TypeSyntax::Kind::Array: {
            auto element =
                build_pattern_term(*syntax.element_type, owner_module,
                                   type_parameters, variable_prefix);
            if (!element) {
                return std::unexpected(element.error());
            }
            PatternTerm term;
            term.kind = PatternTerm::Kind::Array;
            term.array_size = syntax.array_size;
            term.element_type =
                std::make_shared<PatternTerm>(std::move(*element));
            return term;
        }
        }
        return std::unexpected(
            Diagnostic("invalid impl receiver type pattern", syntax.range));
    };

    std::function<PatternTerm(const PatternTerm&, const Substitutions&)>
        resolve_pattern =
            [&](const PatternTerm& term,
                const Substitutions& substitutions) -> PatternTerm {
        if (term.kind != PatternTerm::Kind::Variable) {
            return term;
        }
        const auto it = substitutions.find(term.id);
        if (it == substitutions.end()) {
            return term;
        }
        return resolve_pattern(it->second, substitutions);
    };

    std::function<bool(std::string_view, const PatternTerm&,
                       const Substitutions&)>
        occurs_in_pattern = [&](std::string_view variable,
                                const PatternTerm& raw_term,
                                const Substitutions& substitutions) -> bool {
        const auto term = resolve_pattern(raw_term, substitutions);
        if (term.kind == PatternTerm::Kind::Variable) {
            return term.id == variable;
        }
        if (term.element_type != nullptr &&
            occurs_in_pattern(variable, *term.element_type, substitutions)) {
            return true;
        }
        return std::ranges::any_of(
            term.type_arguments, [&](const auto& type_arg) {
                return occurs_in_pattern(variable, type_arg, substitutions);
            });
    };

    std::function<bool(const PatternTerm&, const PatternTerm&, Substitutions&)>
        unify_patterns = [&](const PatternTerm& lhs_raw,
                             const PatternTerm& rhs_raw,
                             Substitutions& substitutions) -> bool {
        const auto lhs = resolve_pattern(lhs_raw, substitutions);
        const auto rhs = resolve_pattern(rhs_raw, substitutions);
        if (lhs.kind == PatternTerm::Kind::Variable) {
            if (rhs.kind == PatternTerm::Kind::Variable && lhs.id == rhs.id) {
                return true;
            }
            if (occurs_in_pattern(lhs.id, rhs, substitutions)) {
                return false;
            }
            substitutions[lhs.id] = rhs;
            return true;
        }
        if (rhs.kind == PatternTerm::Kind::Variable) {
            if (occurs_in_pattern(rhs.id, lhs, substitutions)) {
                return false;
            }
            substitutions[rhs.id] = lhs;
            return true;
        }
        if (lhs.kind != rhs.kind) {
            return false;
        }
        switch (lhs.kind) {
        case PatternTerm::Kind::Named:
            if (lhs.id != rhs.id ||
                lhs.type_arguments.size() != rhs.type_arguments.size()) {
                return false;
            }
            for (std::size_t index = 0; index < lhs.type_arguments.size();
                 ++index) {
                if (!unify_patterns(lhs.type_arguments[index],
                                    rhs.type_arguments[index], substitutions)) {
                    return false;
                }
            }
            return true;
        case PatternTerm::Kind::Borrow:
            return lhs.is_mut == rhs.is_mut &&
                   unify_patterns(*lhs.element_type, *rhs.element_type,
                                  substitutions);
        case PatternTerm::Kind::Slice:
            return unify_patterns(*lhs.element_type, *rhs.element_type,
                                  substitutions);
        case PatternTerm::Kind::Pointer:
            return unify_patterns(*lhs.element_type, *rhs.element_type,
                                  substitutions);
        case PatternTerm::Kind::Array:
            return lhs.array_size == rhs.array_size &&
                   unify_patterns(*lhs.element_type, *rhs.element_type,
                                  substitutions);
        case PatternTerm::Kind::Variable:
            break;
        }
        return false;
    };

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr ||
                function->impl_target_kind == ast::ImplTargetKind::None) {
                continue;
            }

            ScopedModule scoped_module(active_module, function->owner_module);
            if (function->name == "drop") {
                if (function->return_type == nullptr) {
                    function->return_type =
                        make_type_syntax_from_type(types.voidType());
                }
            } else {
                const auto* interface_decl =
                    findVisibleInterface(function->name);
                if (interface_decl == nullptr) {
                    return std::unexpected(
                        Diagnostic("unknown interface '" + function->name + "'",
                                   function->range));
                }
                function->interface_decl = interface_decl;
                if (function->return_type == nullptr) {
                    function->return_type =
                        clone_type_syntax(*interface_decl->return_type, {});
                }
            }

            const auto interface_key_name =
                function->interface_decl != nullptr
                    ? function->interface_decl->name
                    : std::string(last_qualified_name_segment(function->name));
            const auto* interface_key_module =
                function->interface_decl != nullptr
                    ? function->interface_decl->owner_module
                    : nullptr;

            const auto* receiver_pattern = implReceiverPattern(*function);
            if (receiver_pattern == nullptr) {
                return std::unexpected(Diagnostic(
                    "impl receiver must be the first parameter with type "
                    "'&T' or '&mut T'",
                    function->range));
            }
            function->impl_target_name =
                receiver_pattern->kind == ast::TypeSyntax::Kind::Named
                    ? receiver_pattern->name
                    : describe_type_syntax(*receiver_pattern);

            if (receiver_pattern->kind == ast::TypeSyntax::Kind::Named &&
                receiver_pattern->type_arguments.empty() &&
                std::ranges::find(function->type_parameters,
                                  receiver_pattern->name) !=
                    function->type_parameters.end()) {
                return std::unexpected(
                    Diagnostic("impl receiver cannot be a bare type parameter",
                               receiver_pattern->range));
            }

            if (receiver_pattern->kind == ast::TypeSyntax::Kind::Named) {
                bool has_nominal_target_type = false;
                bool is_builtin_target = false;
                if (const auto* named = findNamedTypeInModule(
                        *function->owner_module, receiver_pattern->name);
                    named != nullptr) {
                    const auto* unqualified = types.unqualify(named);
                    has_nominal_target_type =
                        unqualified->kind == TypeKind::Struct ||
                        unqualified->kind == TypeKind::Enum;
                    is_builtin_target = !has_nominal_target_type;
                }
                has_nominal_target_type =
                    has_nominal_target_type ||
                    findStructTemplateInModule(*function->owner_module,
                                               receiver_pattern->name) !=
                        nullptr ||
                    findEnumTemplateInModule(*function->owner_module,
                                             receiver_pattern->name) != nullptr;
                if (!has_nominal_target_type && !is_builtin_target) {
                    return std::unexpected(
                        Diagnostic("unknown impl target type '" +
                                       function->impl_target_name + "'",
                                   receiver_pattern->range));
                }
                if (function->name == "drop" && !has_nominal_target_type) {
                    return std::unexpected(Diagnostic(
                        "impl drop target must be a struct or enum type",
                        receiver_pattern->range));
                }
            } else if (function->name == "drop") {
                return std::unexpected(
                    Diagnostic("impl drop target must be a struct or enum type",
                               receiver_pattern->range));
            }

            auto receiver_term = build_pattern_term(
                *receiver_pattern, *function->owner_module,
                function->type_parameters, function->linkage_name + "::");
            if (!receiver_term) {
                return std::unexpected(receiver_term.error());
            }

            const auto key = make_impl_key(
                interface_key_name, interface_key_module,
                impl_target_group_key(*receiver_pattern));
            auto& impls = package_impls[key];
            for (auto* existing_impl : impls) {
                auto existing_pattern =
                    build_pattern_term(*implReceiverPattern(*existing_impl),
                                       *existing_impl->owner_module,
                                       existing_impl->type_parameters,
                                       existing_impl->linkage_name + "::");
                if (!existing_pattern) {
                    return std::unexpected(existing_pattern.error());
                }
                Substitutions substitutions;
                if (unify_patterns(*receiver_term, *existing_pattern,
                                   substitutions)) {
                    return std::unexpected(Diagnostic(
                        "duplicate impl for interface '" + interface_key_name +
                            "' on '" + function->impl_target_name + "'",
                        function->range));
                }
            }
            impls.push_back(function);
        }
    }

    return {};
}

auto SemanticAnalyzer::registerPropertyImplDeclarations(ast::Package& package)
    -> std::expected<void, Diagnostic> {
    const auto canonical_property_pattern =
        [](const ast::PropertyImplDecl& decl) -> std::string {
        std::unordered_map<std::string, std::size_t> parameter_indices;
        for (std::size_t index = 0; index < decl.type_parameters.size();
             ++index) {
            parameter_indices.emplace(decl.type_parameters[index], index);
        }
        std::string pattern = decl.impl_target_name;
        if (decl.target_type == nullptr || decl.target_type->type_arguments.empty()) {
            return pattern;
        }
        pattern.push_back('<');
        for (std::size_t index = 0; index < decl.target_type->type_arguments.size();
             ++index) {
            if (index != 0) {
                pattern.push_back(',');
            }
            const auto& arg = *decl.target_type->type_arguments[index];
            const auto it = parameter_indices.find(arg.name);
            if (arg.kind == ast::TypeSyntax::Kind::Named &&
                arg.type_arguments.empty() && it != parameter_indices.end()) {
                pattern.push_back('$');
                pattern += std::to_string(it->second);
            } else {
                pattern += describe_type_syntax(arg);
            }
        }
        pattern.push_back('>');
        return pattern;
    };

    for (const auto& module : package.modules) {
        for (auto& decl : module->declarations) {
            auto* property_decl = std::get_if<ast::PropertyImplDecl>(&decl);
            if (property_decl == nullptr) {
                continue;
            }
            ScopedModule scoped_module(active_module, property_decl->owner_module);

            if (property_decl->target_type == nullptr ||
                property_decl->target_type->kind != ast::TypeSyntax::Kind::Named) {
                return std::unexpected(Diagnostic(
                    "thread property impl target must be a nominal type",
                    property_decl->range));
            }
            const auto& target_name = property_decl->target_type->name;

            bool has_nominal_target_type = false;
            std::optional<std::size_t> expected_type_argument_count;
            const ast::Module* canonical_target_module = nullptr;
            std::string canonical_target_name;
            if (const auto* named = findNamedTypeInModule(
                    *property_decl->owner_module, target_name);
                named != nullptr) {
                const auto* unqualified = types.unqualify(named);
                if (unqualified->kind == TypeKind::Struct &&
                    unqualified->struct_decl != nullptr) {
                    const auto* canonical_decl =
                        unqualified->struct_decl->template_decl != nullptr
                            ? unqualified->struct_decl->template_decl
                            : unqualified->struct_decl;
                    has_nominal_target_type = true;
                    expected_type_argument_count = 0;
                    canonical_target_module = canonical_decl->owner_module;
                    canonical_target_name = canonical_decl->name;
                } else if (unqualified->kind == TypeKind::Enum &&
                           unqualified->enum_decl != nullptr) {
                    const auto* canonical_decl =
                        unqualified->enum_decl->template_decl != nullptr
                            ? unqualified->enum_decl->template_decl
                            : unqualified->enum_decl;
                    has_nominal_target_type = true;
                    expected_type_argument_count = 0;
                    canonical_target_module = canonical_decl->owner_module;
                    canonical_target_name = canonical_decl->name;
                }
            }
            if (auto* struct_template = findStructTemplateInModule(
                    *property_decl->owner_module, target_name);
                struct_template != nullptr) {
                has_nominal_target_type = true;
                expected_type_argument_count =
                    struct_template->type_parameters.size();
                canonical_target_module = struct_template->owner_module;
                canonical_target_name = struct_template->name;
            } else if (auto* enum_template = findEnumTemplateInModule(
                           *property_decl->owner_module,
                           target_name);
                       enum_template != nullptr) {
                has_nominal_target_type = true;
                expected_type_argument_count =
                    enum_template->type_parameters.size();
                canonical_target_module = enum_template->owner_module;
                canonical_target_name = enum_template->name;
            }
            if (!has_nominal_target_type || canonical_target_module == nullptr ||
                canonical_target_name.empty()) {
                return std::unexpected(Diagnostic(
                    "unknown thread property target type '" + target_name + "'",
                    property_decl->target_type->range));
            }
            property_decl->impl_target_module = canonical_target_module;
            property_decl->impl_target_name = canonical_target_name;
            if (property_decl->property_kind == ast::ThreadPropertyKind::Share &&
                !property_decl->is_unchecked) {
                return std::unexpected(Diagnostic(
                    "impl share(...) must be declared unchecked",
                    property_decl->property_name_range));
            }
            if (property_decl->property_kind != ast::ThreadPropertyKind::Share &&
                property_decl->is_unchecked) {
                return std::unexpected(Diagnostic(
                    "only impl share(...) may be declared unchecked",
                    property_decl->property_name_range));
            }
            if (expected_type_argument_count.has_value() &&
                *expected_type_argument_count == 0 &&
                !property_decl->target_type->type_arguments.empty()) {
                return std::unexpected(Diagnostic(
                    "type '" + property_decl->impl_target_name +
                        "' is not generic",
                    property_decl->target_type->range));
            }
            if (expected_type_argument_count.has_value() &&
                *expected_type_argument_count != 0 &&
                property_decl->target_type->type_arguments.empty()) {
                return std::unexpected(Diagnostic(
                    "generic type '" + property_decl->impl_target_name +
                        "' requires explicit type arguments",
                    property_decl->target_type->range));
            }
            if (expected_type_argument_count.has_value() &&
                property_decl->target_type->type_arguments.size() !=
                    *expected_type_argument_count) {
                return std::unexpected(Diagnostic(
                    "wrong number of type arguments for '" +
                        property_decl->impl_target_name + "'",
                    property_decl->target_type->range));
            }

            for (const auto& type_argument :
                 property_decl->target_type->type_arguments) {
                if (type_argument == nullptr) {
                    continue;
                }
                const bool is_bare_type_parameter =
                    type_argument->kind == ast::TypeSyntax::Kind::Named &&
                    type_argument->type_arguments.empty() &&
                    std::ranges::find(property_decl->type_parameters,
                                      type_argument->name) !=
                        property_decl->type_parameters.end();
                if (!is_bare_type_parameter) {
                    return std::unexpected(Diagnostic(
                        "thread property impl target arguments must be bare "
                        "type parameters",
                        type_argument->range));
                }
            }

            const auto pattern = canonical_property_pattern(*property_decl);
            auto& decls = package_property_impls[property_decl->property_kind];
            for (const auto* existing : decls) {
                if (existing->owner_module != property_decl->owner_module ||
                    existing->impl_target_module !=
                        property_decl->impl_target_module ||
                    existing->impl_target_name != property_decl->impl_target_name) {
                    continue;
                }
                if (canonical_property_pattern(*existing) == pattern) {
                    return std::unexpected(Diagnostic(
                        "duplicate thread property impl for '" +
                            property_decl->impl_target_name + "'",
                        property_decl->range));
                }
            }
            decls.push_back(property_decl);
        }
    }

    return {};
}

auto SemanticAnalyzer::registerVisibleDecl(ModuleScope& scope, ast::Decl& decl,
                                           SourceRange conflict_range)
    -> std::expected<void, Diagnostic> {
    auto has_type_conflict = [&](std::string_view name) {
        return scope.import_namespaces.contains(std::string(name)) ||
               scope.structs.contains(std::string(name)) ||
               scope.struct_templates.contains(std::string(name)) ||
               scope.enums.contains(std::string(name)) ||
               scope.enum_templates.contains(std::string(name)) ||
               scope.interfaces.contains(std::string(name)) ||
               scope.functions.contains(std::string(name)) ||
               scope.function_templates.contains(std::string(name));
    };

    if (auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
        struct_decl != nullptr) {
        if (has_type_conflict(struct_decl->name)) {
            return make_error("duplicate declaration for '" +
                                  struct_decl->name + "'",
                              conflict_range);
        }
        if (struct_decl->type_parameters.empty()) {
            scope.structs[struct_decl->name] = struct_decl;
        } else {
            scope.struct_templates[struct_decl->name] = struct_decl;
        }
        return {};
    }

    if (auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
        enum_decl != nullptr) {
        if (has_type_conflict(enum_decl->name)) {
            return make_error("duplicate declaration for '" + enum_decl->name +
                                  "'",
                              conflict_range);
        }
        if (enum_decl->type_parameters.empty()) {
            scope.enums[enum_decl->name] = enum_decl;
        } else {
            scope.enum_templates[enum_decl->name] = enum_decl;
        }

        std::unordered_map<std::string, bool> variant_names;
        for (std::size_t index = 0; index < enum_decl->variants.size();
             ++index) {
            const auto& variant = enum_decl->variants[index];
            if (variant_names.contains(variant.name)) {
                return make_error("duplicate enum variant '" + variant.name +
                                      "'",
                                  variant.range);
            }
            variant_names.emplace(variant.name, true);

            if (enum_decl->type_parameters.empty()) {
                if (scope.variants.contains(variant.name) ||
                    scope.template_variants.contains(variant.name)) {
                    return make_error("duplicate enum variant '" +
                                          variant.name + "'",
                                      conflict_range);
                }
                scope.variants[variant.name] = std::make_pair(enum_decl, index);
            } else {
                if (scope.variants.contains(variant.name)) {
                    return make_error("duplicate enum variant '" +
                                          variant.name + "'",
                                      conflict_range);
                }
                scope.template_variants[variant.name].emplace_back(enum_decl,
                                                                   index);
            }
        }
        return {};
    }

    if (auto* interface_decl = std::get_if<ast::InterfaceDecl>(&decl);
        interface_decl != nullptr) {
        if (has_type_conflict(interface_decl->name)) {
            return make_error("duplicate declaration for '" +
                                  interface_decl->name + "'",
                              conflict_range);
        }
        scope.interfaces[interface_decl->name] = interface_decl;
        return {};
    }

    auto* function_decl = std::get_if<ast::FunctionDecl>(&decl);
    if (function_decl == nullptr) {
        if (std::get_if<ast::PropertyImplDecl>(&decl) != nullptr) {
            return {};
        }
        return {};
    }
    if (function_decl->impl_target_kind != ast::ImplTargetKind::None) {
        return {};
    }
    if (has_type_conflict(function_decl->name)) {
        return make_error("duplicate declaration for '" + function_decl->name +
                              "'",
                          conflict_range);
    }
    if (function_decl->type_parameters.empty()) {
        scope.functions[function_decl->name] = function_decl;
    } else {
        scope.function_templates[function_decl->name] = function_decl;
    }
    return {};
}

auto SemanticAnalyzer::registerImportNamespace(ModuleScope& scope,
                                               std::string name,
                                               ast::Module* module,
                                               SourceRange conflict_range)
    -> std::expected<void, Diagnostic> {
    if (scope.import_namespaces.contains(name) ||
        scope.structs.contains(name) || scope.struct_templates.contains(name) ||
        scope.enums.contains(name) || scope.enum_templates.contains(name) ||
        scope.interfaces.contains(name) || scope.functions.contains(name) ||
        scope.function_templates.contains(name)) {
        return make_error("duplicate declaration for '" + name + "'",
                          conflict_range);
    }
    scope.import_namespaces.emplace(std::move(name), module);
    return {};
}

auto SemanticAnalyzer::mergeImportedScope(ModuleScope& target,
                                          const ModuleScope& source,
                                          SourceRange conflict_range)
    -> std::expected<void, Diagnostic> {
    for (const auto& [name, module] : source.import_namespaces) {
        auto registered =
            registerImportNamespace(target, name, module, conflict_range);
        if (!registered) {
            return std::unexpected(registered.error());
        }
    }

    auto has_type_conflict = [&](std::string_view name) {
        return target.import_namespaces.contains(std::string(name)) ||
               target.structs.contains(std::string(name)) ||
               target.struct_templates.contains(std::string(name)) ||
               target.enums.contains(std::string(name)) ||
               target.enum_templates.contains(std::string(name)) ||
               target.interfaces.contains(std::string(name)) ||
               target.functions.contains(std::string(name)) ||
               target.function_templates.contains(std::string(name));
    };

    for (const auto& [name, decl] : source.structs) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.structs.emplace(name, decl);
    }
    for (const auto& [name, decl] : source.struct_templates) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.struct_templates.emplace(name, decl);
    }
    for (const auto& [name, decl] : source.enums) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.enums.emplace(name, decl);
    }
    for (const auto& [name, decl] : source.enum_templates) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.enum_templates.emplace(name, decl);
    }
    for (const auto& [name, decl] : source.interfaces) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.interfaces.emplace(name, decl);
    }
    for (const auto& [name, decl] : source.functions) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.functions.emplace(name, decl);
    }
    for (const auto& [name, decl] : source.function_templates) {
        if (has_type_conflict(name)) {
            return make_error("duplicate declaration for '" + name + "'",
                              conflict_range);
        }
        target.function_templates.emplace(name, decl);
    }

    for (const auto& [name, variant] : source.variants) {
        if (target.variants.contains(name) ||
            target.template_variants.contains(name)) {
            return make_error("duplicate enum variant '" + name + "'",
                              conflict_range);
        }
        target.variants.emplace(name, variant);
    }
    for (const auto& [name, variants] : source.template_variants) {
        if (target.variants.contains(name)) {
            return make_error("duplicate enum variant '" + name + "'",
                              conflict_range);
        }
        auto& target_variants = target.template_variants[name];
        target_variants.insert(target_variants.end(), variants.begin(),
                               variants.end());
    }

    return {};
}

auto SemanticAnalyzer::visibleScopeFor(const ast::Module& module) const
    -> const ModuleScope& {
    return visible_scopes.at(&module);
}

auto SemanticAnalyzer::localScopeFor(const ast::Module& module) const
    -> const ModuleScope& {
    return local_scopes.at(&module);
}

auto SemanticAnalyzer::exportScopeFor(const ast::Module& module) const
    -> const ModuleScope& {
    return export_scopes.at(&module);
}

auto SemanticAnalyzer::findNamedTypeInModule(const ast::Module& module,
                                             std::string_view name) const
    -> const Type* {
    if (name.find('.') == std::string_view::npos) {
        if (const auto* builtin = types.findNamed(name); builtin != nullptr) {
            return builtin;
        }
    }

    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return nullptr;
    }

    std::function<const Type*(const ModuleScope&, std::size_t)> resolve =
        [&](const ModuleScope& scope, std::size_t index) -> const Type* {
        if (index + 1 == parts.size()) {
            if (const auto struct_it = scope.structs.find(parts[index]);
                struct_it != scope.structs.end()) {
                return struct_it->second->resolved_type;
            }
            if (const auto enum_it = scope.enums.find(parts[index]);
                enum_it != scope.enums.end()) {
                return enum_it->second->resolved_type;
            }
            return nullptr;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return nullptr;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findStructTemplateInModule(const ast::Module& module,
                                                  std::string_view name) const
    -> ast::StructDecl* {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return nullptr;
    }

    std::function<ast::StructDecl*(const ModuleScope&, std::size_t)> resolve =
        [&](const ModuleScope& scope, std::size_t index) -> ast::StructDecl* {
        if (index + 1 == parts.size()) {
            if (const auto it = scope.struct_templates.find(parts[index]);
                it != scope.struct_templates.end()) {
                return it->second;
            }
            return nullptr;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return nullptr;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findEnumTemplateInModule(const ast::Module& module,
                                                std::string_view name) const
    -> ast::EnumDecl* {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return nullptr;
    }

    std::function<ast::EnumDecl*(const ModuleScope&, std::size_t)> resolve =
        [&](const ModuleScope& scope, std::size_t index) -> ast::EnumDecl* {
        if (index + 1 == parts.size()) {
            if (const auto it = scope.enum_templates.find(parts[index]);
                it != scope.enum_templates.end()) {
                return it->second;
            }
            return nullptr;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return nullptr;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findInterfaceInModule(const ast::Module& module,
                                             std::string_view name) const
    -> const ast::InterfaceDecl* {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return nullptr;
    }

    std::function<const ast::InterfaceDecl*(const ModuleScope&, std::size_t)>
        resolve = [&](const ModuleScope& scope,
                      std::size_t index) -> const ast::InterfaceDecl* {
        if (index + 1 == parts.size()) {
            if (const auto it = scope.interfaces.find(parts[index]);
                it != scope.interfaces.end()) {
                return it->second;
            }
            return nullptr;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return nullptr;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findFunctionInModule(const ast::Module& module,
                                            std::string_view name) const
    -> ast::FunctionDecl* {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return nullptr;
    }

    std::function<ast::FunctionDecl*(const ModuleScope&, std::size_t)> resolve =
        [&](const ModuleScope& scope, std::size_t index) -> ast::FunctionDecl* {
        if (index + 1 == parts.size()) {
            if (const auto it = scope.functions.find(parts[index]);
                it != scope.functions.end()) {
                return it->second;
            }
            return nullptr;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return nullptr;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findFunctionTemplateInModule(const ast::Module& module,
                                                    std::string_view name) const
    -> ast::FunctionDecl* {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return nullptr;
    }

    std::function<ast::FunctionDecl*(const ModuleScope&, std::size_t)> resolve =
        [&](const ModuleScope& scope, std::size_t index) -> ast::FunctionDecl* {
        if (index + 1 == parts.size()) {
            if (const auto it = scope.function_templates.find(parts[index]);
                it != scope.function_templates.end()) {
                return it->second;
            }
            return nullptr;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return nullptr;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findEnumVariantInModule(const ast::Module& module,
                                               std::string_view name) const
    -> std::optional<std::pair<ast::EnumDecl*, std::size_t>> {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return std::nullopt;
    }

    std::function<std::optional<std::pair<ast::EnumDecl*, std::size_t>>(
        const ModuleScope&, std::size_t)>
        resolve = [&](const ModuleScope& scope, std::size_t index)
        -> std::optional<std::pair<ast::EnumDecl*, std::size_t>> {
        if (index + 1 == parts.size()) {
            if (const auto it = scope.variants.find(parts[index]);
                it != scope.variants.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        const auto namespace_it = scope.import_namespaces.find(parts[index]);
        if (namespace_it == scope.import_namespaces.end()) {
            return std::nullopt;
        }
        return resolve(exportScopeFor(*namespace_it->second), index + 1);
    };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findTemplateEnumVariantsInModule(
    const ast::Module& module, std::string_view name) const
    -> std::vector<std::pair<ast::EnumDecl*, std::size_t>> {
    const auto parts = split_qualified_name(name);
    if (parts.empty()) {
        return {};
    }

    std::function<std::vector<std::pair<ast::EnumDecl*, std::size_t>>(
        const ModuleScope&, std::size_t)>
        resolve = [&](const ModuleScope& scope, std::size_t index) {
            if (index + 1 == parts.size()) {
                if (const auto it = scope.template_variants.find(parts[index]);
                    it != scope.template_variants.end()) {
                    return it->second;
                }
                return std::vector<std::pair<ast::EnumDecl*, std::size_t>>{};
            }

            const auto namespace_it =
                scope.import_namespaces.find(parts[index]);
            if (namespace_it == scope.import_namespaces.end()) {
                return std::vector<std::pair<ast::EnumDecl*, std::size_t>>{};
            }
            return resolve(exportScopeFor(*namespace_it->second), index + 1);
        };

    return resolve(visibleScopeFor(module), 0);
}

auto SemanticAnalyzer::findVisibleNamedType(std::string_view name) const
    -> const Type* {
    if (active_module == nullptr) {
        return nullptr;
    }
    return findNamedTypeInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleStructTemplate(std::string_view name) const
    -> ast::StructDecl* {
    if (active_module == nullptr) {
        return nullptr;
    }
    return findStructTemplateInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleEnumTemplate(std::string_view name) const
    -> ast::EnumDecl* {
    if (active_module == nullptr) {
        return nullptr;
    }
    return findEnumTemplateInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleInterface(std::string_view name) const
    -> const ast::InterfaceDecl* {
    if (active_module == nullptr) {
        return nullptr;
    }
    return findInterfaceInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleFunction(std::string_view name) const
    -> ast::FunctionDecl* {
    if (active_module == nullptr) {
        return nullptr;
    }
    return findFunctionInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleFunctionTemplate(std::string_view name) const
    -> ast::FunctionDecl* {
    if (active_module == nullptr) {
        return nullptr;
    }
    return findFunctionTemplateInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleEnumVariant(std::string_view name) const
    -> std::optional<std::pair<ast::EnumDecl*, std::size_t>> {
    if (active_module == nullptr) {
        return std::nullopt;
    }
    return findEnumVariantInModule(*active_module, name);
}

auto SemanticAnalyzer::findVisibleTemplateEnumVariants(std::string_view name)
    const -> std::vector<std::pair<ast::EnumDecl*, std::size_t>> {
    if (active_module == nullptr) {
        return {};
    }
    return findTemplateEnumVariantsInModule(*active_module, name);
}

auto SemanticAnalyzer::validateExternSignature(const ast::FunctionDecl& decl)
    -> std::expected<void, Diagnostic> {
    if (decl.intrinsic_lowering.has_value() && !decl.is_extern) {
        return std::unexpected(Diagnostic(
            "lowering directives are only supported on extern functions",
            decl.intrinsic_lowering->range));
    }
    if (!decl.is_extern) {
        return {};
    }
    if (!decl.type_parameters.empty()) {
        return std::unexpected(Diagnostic(
            "extern functions do not support type parameters", decl.range));
    }
    if (!decl.declared_return_dependencies.empty()) {
        return std::unexpected(Diagnostic(
            "extern functions do not support depends clauses", decl.range));
    }
    if (decl.resolved_return_type == nullptr ||
        std::ranges::any_of(decl.parameters, [](const auto& parameter) {
            return parameter.resolved_type == nullptr;
        })) {
        return {};
    }

    const auto is_ffi_scalar = [&](const Type* type) {
        type = types.unqualify(type);
        return type->kind == TypeKind::Integer || type->kind == TypeKind::Float;
    };
    const auto is_ffi_type = [&](const Type* type) {
        type = types.unqualify(type);
        return type == types.voidType() || is_ffi_scalar(type) ||
               type->kind == TypeKind::Pointer;
    };

    if (!is_ffi_type(decl.resolved_return_type)) {
        return std::unexpected(Diagnostic(
            "extern return types are limited to void, integers, floats, and "
            "raw pointers",
            decl.return_type != nullptr ? decl.return_type->range
                                        : decl.range));
    }
    if (decl.intrinsic_lowering.has_value() &&
        decl.intrinsic_lowering->constant_kind ==
            ast::LoweringConstantKind::NullValue &&
        types.unqualify(decl.resolved_return_type) == types.voidType()) {
        return std::unexpected(
            Diagnostic("constant=null lowering requires a non-void return type",
                       decl.return_type != nullptr ? decl.return_type->range
                                                   : decl.range));
    }
    for (const auto& parameter : decl.parameters) {
        if (!is_ffi_type(parameter.resolved_type) ||
            types.unqualify(parameter.resolved_type) == types.voidType()) {
            return std::unexpected(Diagnostic(
                "extern parameter types are limited to integers, floats, and "
                "raw pointers",
                parameter.range));
        }
    }
    return {};
}

auto SemanticAnalyzer::ensureFunctionSignature(ast::FunctionDecl& decl)
    -> std::expected<void, Diagnostic> {
    ScopedModule scoped_module(active_module, decl.owner_module);

    if (decl.impl_target_kind != ast::ImplTargetKind::None) {
        if (decl.name == "drop") {
            if (decl.return_type == nullptr) {
                decl.return_type = make_type_syntax_from_type(types.voidType());
            }
        } else {
            if (decl.interface_decl == nullptr) {
                const auto* interface_decl = findVisibleInterface(decl.name);
                if (interface_decl == nullptr) {
                    return make_error("unknown interface '" + decl.name + "'",
                                      decl.range);
                }
                decl.interface_decl = interface_decl;
            }
            if (decl.return_type == nullptr) {
                decl.return_type =
                    clone_type_syntax(*decl.interface_decl->return_type, {});
            }
        }
    }

    if (decl.return_type == nullptr) {
        return make_error("impl is missing its interface return type",
                          decl.range);
    }

    if (decl.resolved_return_type == nullptr) {
        auto return_type = resolveType(*decl.return_type);
        if (!return_type) {
            return std::unexpected(return_type.error());
        }
        decl.resolved_return_type = *return_type;
    }

    for (auto& parameter : decl.parameters) {
        if (parameter.resolved_type != nullptr) {
            continue;
        }
        auto parameter_type = resolveType(*parameter.type);
        if (!parameter_type) {
            return std::unexpected(parameter_type.error());
        }
        parameter.resolved_type = *parameter_type;
    }

    auto validated_dependency = validateReturnDependencies(decl);
    if (!validated_dependency) {
        return std::unexpected(validated_dependency.error());
    }

    if (decl.is_extern) {
        auto validated_extern = validateExternSignature(decl);
        if (!validated_extern) {
            return std::unexpected(validated_extern.error());
        }
    }

    if (decl.impl_target_kind != ast::ImplTargetKind::None) {
        const auto* target_type = interfaceReceiverType(
            decl.parameters.empty() ? nullptr
                                    : decl.parameters.front().resolved_type);
        auto validated_impl = validateResolvedImplSignature(decl, target_type);
        if (!validated_impl) {
            return std::unexpected(validated_impl.error());
        }
        if (decl.name == "drop") {
            types.registerDropFunction(target_type, &decl);
        }
    }

    return {};
}

auto SemanticAnalyzer::implReceiverPattern(const ast::FunctionDecl& decl) const
    -> const ast::TypeSyntax* {
    if (decl.parameters.empty() || decl.parameters.front().type == nullptr) {
        return nullptr;
    }
    const auto* receiver_type = decl.parameters.front().type.get();
    if (receiver_type->kind != ast::TypeSyntax::Kind::Borrow ||
        receiver_type->element_type == nullptr) {
        return nullptr;
    }
    return receiver_type->element_type.get();
}

auto SemanticAnalyzer::analyzeStruct(ast::StructDecl& decl)
    -> std::expected<void, Diagnostic> {
    ScopedModule scoped_module(active_module, decl.owner_module);
    std::unordered_map<std::string, bool> field_names;
    bool had_error = false;
    for (auto& field : decl.fields) {
        if (field_names.contains(field.name)) {
            report(Diagnostic("duplicate field '" + field.name + "'",
                              field.range));
            had_error = true;
            continue;
        }
        field_names.emplace(field.name, true);

        auto field_type = resolveType(*field.type);
        if (!field_type) {
            report(field_type.error());
            had_error = true;
            continue;
        }
        field.resolved_type = *field_type;
    }
    if (had_error) {
        return make_error("struct analysis failed", decl.range);
    }
    return {};
}

auto SemanticAnalyzer::analyzeInterface(ast::InterfaceDecl& decl)
    -> std::expected<void, Diagnostic> {
    ScopedModule scoped_module(active_module, decl.owner_module);
    bool had_error = false;
    if (decl.parameters.empty()) {
        report(Diagnostic("interface must declare a receiver parameter",
                          decl.range));
        had_error = true;
    }
    if (decl.return_type == nullptr) {
        report(Diagnostic("interface is missing a return type", decl.range));
        had_error = true;
    }
    if (had_error) {
        return make_error("interface analysis failed", decl.range);
    }

    const auto* receiver_type = decl.parameters.front().type.get();
    if (receiver_type == nullptr ||
        receiver_type->kind != ast::TypeSyntax::Kind::Borrow ||
        receiver_type->element_type == nullptr ||
        receiver_type->element_type->kind != ast::TypeSyntax::Kind::Named ||
        receiver_type->element_type->name != decl.receiver_type_parameter ||
        !receiver_type->element_type->type_arguments.empty()) {
        report(Diagnostic(
            "interface receiver must be the first parameter with type &T or "
            "&mut T",
            decl.parameters.front().range));
        had_error = true;
    }
    if (!had_error) {
        decl.receiver_is_mut = receiver_type->is_mut;
    }

    if (type_syntax_contains_name(*decl.return_type,
                                  decl.receiver_type_parameter)) {
        report(Diagnostic(
            "interface return type cannot mention the receiver type parameter",
            decl.return_type->range));
        had_error = true;
    }

    auto return_type = resolveType(*decl.return_type);
    if (!return_type) {
        report(return_type.error());
        had_error = true;
    } else {
        decl.resolved_return_type = *return_type;
        types.getInterface(&decl);
    }

    decl.parameters.front().resolved_type =
        had_error ? nullptr : types.getInterface(&decl);
    for (std::size_t index = 1; index < decl.parameters.size(); ++index) {
        auto& parameter = decl.parameters[index];
        if (type_syntax_contains_name(*parameter.type,
                                      decl.receiver_type_parameter)) {
            report(Diagnostic(
                "only the receiver parameter may mention the interface type "
                "parameter",
                parameter.range));
            had_error = true;
            continue;
        }
        auto parameter_type = resolveType(*parameter.type);
        if (!parameter_type) {
            report(parameter_type.error());
            had_error = true;
            continue;
        }
        parameter.resolved_type = *parameter_type;
    }
    if (had_error) {
        return make_error("interface analysis failed", decl.range);
    }
    return {};
}

auto SemanticAnalyzer::analyzeEnum(ast::EnumDecl& decl)
    -> std::expected<void, Diagnostic> {
    ScopedModule scoped_module(active_module, decl.owner_module);
    std::unordered_map<std::string, bool> variant_names;
    bool had_error = false;
    for (std::size_t index = 0; index < decl.variants.size(); ++index) {
        auto& variant = decl.variants[index];
        if (variant_names.contains(variant.name)) {
            report(Diagnostic("duplicate enum variant '" + variant.name + "'",
                              variant.range));
            had_error = true;
            continue;
        }
        variant_names.emplace(variant.name, true);

        if (variant.payload_type == nullptr) {
            continue;
        }

        auto payload_type = resolveType(*variant.payload_type);
        if (!payload_type) {
            report(payload_type.error());
            had_error = true;
            continue;
        }
        variant.resolved_type = *payload_type;
    }
    if (had_error) {
        return make_error("enum analysis failed", decl.range);
    }
    return {};
}

auto SemanticAnalyzer::report(Diagnostic diagnostic) -> void {
    diagnostics.push_back(std::move(diagnostic));
}

} // namespace cyan

#include "sema_detail.hpp"

namespace cyan {

using namespace detail;

SemanticAnalyzer::SemanticAnalyzer(TypeContext& types) : types(types) {}

auto SemanticAnalyzer::exportScope(const ModuleScope& scope) const
    -> SemanticScope {
    SemanticScope exported;
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
    visible_scopes.clear();
    package_impls.clear();
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

            const auto* interface_decl = findVisibleInterface(function->name);
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
        for (auto& decl : module->declarations) {
            auto* function = std::get_if<ast::FunctionDecl>(&decl);
            if (function == nullptr || !function->type_parameters.empty()) {
                continue;
            }

            ScopedModule scoped_module(active_module, function->owner_module);
            if (function->return_type == nullptr) {
                report(Diagnostic("impl is missing its interface return type",
                                  function->range));
                continue;
            }
            auto return_type = resolveType(*function->return_type);
            if (!return_type) {
                report(return_type.error());
                continue;
            }
            function->resolved_return_type = *return_type;

            bool has_parameter_errors = false;
            for (auto& parameter : function->parameters) {
                auto parameter_type = resolveType(*parameter.type);
                if (!parameter_type) {
                    report(parameter_type.error());
                    has_parameter_errors = true;
                    continue;
                }
                parameter.resolved_type = *parameter_type;
            }
            if (has_parameter_errors) {
                continue;
            }

            auto validated_dependency = validateReturnDependencies(*function);
            if (!validated_dependency) {
                report(validated_dependency.error());
                continue;
            }

            if (function->impl_target_kind != ast::ImplTargetKind::None) {
                const auto* target_type = interfaceReceiverType(
                    function->parameters.empty()
                        ? nullptr
                        : function->parameters.front().resolved_type);
                auto validated_impl =
                    validateResolvedImplSignature(*function, target_type);
                if (!validated_impl) {
                    report(validated_impl.error());
                    continue;
                }
                if (function->name == "drop") {
                    types.registerDropFunction(target_type, function);
                }
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
    visible_scopes = local_scopes;
    for (const auto& module : package.modules) {
        auto& visible_scope = visible_scopes[module.get()];
        for (const auto& import_decl : module->imports) {
            if (import_decl.imported_module == nullptr) {
                report(Diagnostic("unresolved import '" +
                                      import_decl.module_name + "'",
                                  import_decl.range));
                continue;
            }
            for (auto& decl : import_decl.imported_module->declarations) {
                auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
                if (struct_decl != nullptr && struct_decl->is_export) {
                    auto registered = registerVisibleDecl(visible_scope, decl,
                                                          import_decl.range);
                    if (!registered) {
                        report(registered.error());
                    }
                    continue;
                }
                auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
                if (enum_decl != nullptr && enum_decl->is_export) {
                    auto registered = registerVisibleDecl(visible_scope, decl,
                                                          import_decl.range);
                    if (!registered) {
                        report(registered.error());
                    }
                    continue;
                }
                auto* interface_decl = std::get_if<ast::InterfaceDecl>(&decl);
                if (interface_decl != nullptr && interface_decl->is_export) {
                    auto registered = registerVisibleDecl(visible_scope, decl,
                                                          import_decl.range);
                    if (!registered) {
                        report(registered.error());
                    }
                    continue;
                }
                auto* function_decl = std::get_if<ast::FunctionDecl>(&decl);
                if (function_decl != nullptr && function_decl->is_export) {
                    auto registered = registerVisibleDecl(visible_scope, decl,
                                                          import_decl.range);
                    if (!registered) {
                        report(registered.error());
                    }
                }
            }
        }
    }
    return {};
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
            const auto& scope = visibleScopeFor(owner_module);
            if (syntax.type_arguments.empty()) {
                if (const auto* builtin = types.findNamed(syntax.name);
                    builtin != nullptr) {
                    term.id = builtin->linkage_name;
                } else if (const auto struct_it =
                               scope.structs.find(syntax.name);
                           struct_it != scope.structs.end()) {
                    term.id = struct_it->second->linkage_name;
                } else if (const auto enum_it = scope.enums.find(syntax.name);
                           enum_it != scope.enums.end()) {
                    term.id = enum_it->second->linkage_name;
                } else if (scope.struct_templates.contains(syntax.name) ||
                           scope.enum_templates.contains(syntax.name)) {
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

            if (const auto struct_it = scope.struct_templates.find(syntax.name);
                struct_it != scope.struct_templates.end()) {
                term.id = struct_it->second->linkage_name;
            } else if (const auto enum_it =
                           scope.enum_templates.find(syntax.name);
                       enum_it != scope.enum_templates.end()) {
                term.id = enum_it->second->linkage_name;
            } else if (types.findNamed(syntax.name) != nullptr ||
                       scope.structs.contains(syntax.name) ||
                       scope.enums.contains(syntax.name)) {
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

            const auto& scope = visibleScopeFor(*function->owner_module);
            if (receiver_pattern->kind == ast::TypeSyntax::Kind::Named) {
                const auto has_nominal_target_type =
                    scope.structs.contains(receiver_pattern->name) ||
                    scope.struct_templates.contains(receiver_pattern->name) ||
                    scope.enums.contains(receiver_pattern->name) ||
                    scope.enum_templates.contains(receiver_pattern->name);
                const auto is_builtin_target =
                    types.findNamed(receiver_pattern->name) != nullptr;
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
                function->name, impl_target_group_key(*receiver_pattern));
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
                        "duplicate impl for interface '" + function->name +
                            "' on '" + function->impl_target_name + "'",
                        function->range));
                }
            }
            impls.push_back(function);
        }
    }

    return {};
}

auto SemanticAnalyzer::registerVisibleDecl(ModuleScope& scope, ast::Decl& decl,
                                           SourceRange conflict_range)
    -> std::expected<void, Diagnostic> {
    auto has_type_conflict = [&](std::string_view name) {
        return scope.structs.contains(std::string(name)) ||
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

auto SemanticAnalyzer::visibleScopeFor(const ast::Module& module) const
    -> const ModuleScope& {
    return visible_scopes.at(&module);
}

auto SemanticAnalyzer::localScopeFor(const ast::Module& module) const
    -> const ModuleScope& {
    return local_scopes.at(&module);
}

auto SemanticAnalyzer::findVisibleNamedType(std::string_view name) const
    -> const Type* {
    if (const auto* builtin = types.findNamed(name); builtin != nullptr) {
        return builtin;
    }

    if (active_module == nullptr) {
        return nullptr;
    }
    const auto& scope = visibleScopeFor(*active_module);
    if (const auto struct_it = scope.structs.find(std::string(name));
        struct_it != scope.structs.end()) {
        return struct_it->second->resolved_type;
    }
    if (const auto enum_it = scope.enums.find(std::string(name));
        enum_it != scope.enums.end()) {
        return enum_it->second->resolved_type;
    }
    return nullptr;
}

auto SemanticAnalyzer::findVisibleInterface(std::string_view name) const
    -> const ast::InterfaceDecl* {
    if (active_module == nullptr) {
        return nullptr;
    }
    const auto& scope = visibleScopeFor(*active_module);
    if (const auto it = scope.interfaces.find(std::string(name));
        it != scope.interfaces.end()) {
        return it->second;
    }
    return nullptr;
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

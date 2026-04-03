#include "sema_detail.hpp"

#include <unordered_map>
#include <unordered_set>

namespace cyan {

using namespace detail;

auto SemanticAnalyzer::appendInstantiatedDecl(ast::Decl decl) -> ast::Decl* {
    current_package->instantiated_declarations.push_back(
        std::make_unique<ast::Decl>(std::move(decl)));
    return current_package->instantiated_declarations.back().get();
}

auto SemanticAnalyzer::instantiateStructTemplate(
    ast::StructDecl& decl, const std::vector<const Type*>& type_arguments)
    -> std::expected<ast::StructDecl*, Diagnostic> {
    const auto readable_name =
        make_readable_type_name(decl.name, type_arguments, types);
    const auto linkage_name =
        mangle_instantiation_name(decl.linkage_name, type_arguments);
    if (const auto instantiated_it = instantiated_structs.find(linkage_name);
        instantiated_it != instantiated_structs.end()) {
        return instantiated_it->second;
    }

    TypeBindings type_bindings;
    for (std::size_t index = 0; index < decl.type_parameters.size(); ++index) {
        type_bindings.emplace(decl.type_parameters[index],
                              type_arguments[index]);
    }

    ast::StructDecl instantiated_decl;
    instantiated_decl.range = decl.range;
    instantiated_decl.name = readable_name;
    instantiated_decl.name_range = decl.name_range;
    instantiated_decl.linkage_name = linkage_name;
    instantiated_decl.template_decl = &decl;
    instantiated_decl.type_arguments = type_arguments;
    instantiated_decl.owner_module = decl.owner_module;
    for (const auto& field : decl.fields) {
        ast::StructField instantiated_field;
        instantiated_field.range = field.range;
        instantiated_field.name = field.name;
        instantiated_field.name_range = field.name_range;
        instantiated_field.type = clone_type_syntax(*field.type, type_bindings);
        instantiated_decl.fields.push_back(std::move(instantiated_field));
    }

    auto* stored_decl = std::get_if<ast::StructDecl>(
        appendInstantiatedDecl(std::move(instantiated_decl)));
    stored_decl->resolved_type = types.registerStruct(
        stored_decl->name, stored_decl->linkage_name, stored_decl);
    instantiated_structs[stored_decl->linkage_name] = stored_decl;

    auto analyzed = analyzeStruct(*stored_decl);
    if (!analyzed) {
        return std::unexpected(analyzed.error());
    }
    return stored_decl;
}

auto SemanticAnalyzer::instantiateEnumTemplate(
    ast::EnumDecl& decl, const std::vector<const Type*>& type_arguments)
    -> std::expected<ast::EnumDecl*, Diagnostic> {
    const auto readable_name =
        make_readable_type_name(decl.name, type_arguments, types);
    const auto linkage_name =
        mangle_instantiation_name(decl.linkage_name, type_arguments);
    if (const auto instantiated_it = instantiated_enums.find(linkage_name);
        instantiated_it != instantiated_enums.end()) {
        return instantiated_it->second;
    }

    TypeBindings type_bindings;
    for (std::size_t index = 0; index < decl.type_parameters.size(); ++index) {
        type_bindings.emplace(decl.type_parameters[index],
                              type_arguments[index]);
    }

    ast::EnumDecl instantiated_decl;
    instantiated_decl.range = decl.range;
    instantiated_decl.name = readable_name;
    instantiated_decl.name_range = decl.name_range;
    instantiated_decl.linkage_name = linkage_name;
    instantiated_decl.template_decl = &decl;
    instantiated_decl.type_arguments = type_arguments;
    instantiated_decl.owner_module = decl.owner_module;
    for (const auto& variant : decl.variants) {
        ast::EnumVariant instantiated_variant;
        instantiated_variant.range = variant.range;
        instantiated_variant.name = variant.name;
        instantiated_variant.name_range = variant.name_range;
        instantiated_variant.tag = variant.tag;
        if (variant.payload_type != nullptr) {
            instantiated_variant.payload_type =
                clone_type_syntax(*variant.payload_type, type_bindings);
        }
        instantiated_decl.variants.push_back(std::move(instantiated_variant));
    }

    auto* stored_decl = std::get_if<ast::EnumDecl>(
        appendInstantiatedDecl(std::move(instantiated_decl)));
    stored_decl->resolved_type = types.registerEnum(
        stored_decl->name, stored_decl->linkage_name, stored_decl);
    instantiated_enums[stored_decl->linkage_name] = stored_decl;

    auto analyzed = analyzeEnum(*stored_decl);
    if (!analyzed) {
        return std::unexpected(analyzed.error());
    }
    return stored_decl;
}

auto SemanticAnalyzer::instantiateFunctionTemplate(
    ast::FunctionDecl& decl, const TypeBindings& type_bindings)
    -> std::expected<ast::FunctionDecl*, Diagnostic> {
    std::vector<const Type*> type_arguments;
    type_arguments.reserve(decl.type_parameters.size());
    for (const auto& type_parameter : decl.type_parameters) {
        const auto binding_it = type_bindings.find(type_parameter);
        if (binding_it == type_bindings.end()) {
            return unexpected_result<ast::FunctionDecl*>(
                "could not infer type argument for '" + type_parameter + "'",
                decl.range);
        }
        type_arguments.push_back(binding_it->second);
    }

    const auto mangled_name =
        mangle_instantiation_name(decl.linkage_name, type_arguments);
    if (const auto instantiated_it = instantiated_functions.find(mangled_name);
        instantiated_it != instantiated_functions.end()) {
        return instantiated_it->second;
    }

    ast::FunctionDecl instantiated_decl;
    instantiated_decl.range = decl.range;
    instantiated_decl.name = decl.name;
    instantiated_decl.name_range = decl.name_range;
    instantiated_decl.linkage_name = mangled_name;
    instantiated_decl.template_decl = &decl;
    instantiated_decl.type_arguments = type_arguments;
    if (decl.return_type != nullptr) {
        instantiated_decl.return_type =
            clone_type_syntax(*decl.return_type, type_bindings);
    }
    instantiated_decl.declared_return_dependencies =
        decl.declared_return_dependencies;
    if (!instantiated_decl.declared_return_dependencies.empty()) {
        instantiated_decl.return_dependencies =
            instantiated_decl.declared_return_dependencies;
    } else {
        instantiated_decl.return_dependencies = decl.return_dependencies;
    }
    instantiated_decl.impl_target_kind = decl.impl_target_kind;
    instantiated_decl.impl_target_name = decl.impl_target_name;
    instantiated_decl.interface_decl = decl.interface_decl;
    instantiated_decl.intrinsic_lowering = decl.intrinsic_lowering;
    instantiated_decl.builtin_lowering = decl.builtin_lowering;
    instantiated_decl.is_unchecked = decl.is_unchecked;
    instantiated_decl.is_export = decl.is_export;
    instantiated_decl.is_extern = decl.is_extern;
    instantiated_decl.owner_module = decl.owner_module;
    for (const auto& parameter : decl.parameters) {
        ast::Parameter instantiated_parameter;
        instantiated_parameter.range = parameter.range;
        instantiated_parameter.name = parameter.name;
        instantiated_parameter.name_range = parameter.name_range;
        instantiated_parameter.type =
            clone_type_syntax(*parameter.type, type_bindings);
        instantiated_decl.parameters.push_back(
            std::move(instantiated_parameter));
    }
    if (decl.body != nullptr) {
        instantiated_decl.body = clone_block(*decl.body, type_bindings);
    }

    auto* stored_decl = std::get_if<ast::FunctionDecl>(
        appendInstantiatedDecl(std::move(instantiated_decl)));
    instantiated_functions[stored_decl->linkage_name] = stored_decl;

    ScopedModule scoped_module(active_module, stored_decl->owner_module);
    if (stored_decl->impl_target_kind != ast::ImplTargetKind::None &&
        stored_decl->name != "drop" && stored_decl->interface_decl != nullptr &&
        stored_decl->return_type == nullptr) {
        stored_decl->return_type =
            clone_type_syntax(*stored_decl->interface_decl->return_type, {});
    }
    if (stored_decl->impl_target_kind != ast::ImplTargetKind::None &&
        stored_decl->name == "drop" && stored_decl->return_type == nullptr) {
        stored_decl->return_type = make_type_syntax_from_type(types.voidType());
    }
    if (stored_decl->return_type == nullptr) {
        return unexpected_result<ast::FunctionDecl*>(
            "instantiated function is missing a return type",
            stored_decl->range);
    }
    auto return_type = resolveType(*stored_decl->return_type);
    if (!return_type) {
        return std::unexpected(return_type.error());
    }
    stored_decl->resolved_return_type = *return_type;
    for (auto& parameter : stored_decl->parameters) {
        auto parameter_type = resolveType(*parameter.type);
        if (!parameter_type) {
            return std::unexpected(parameter_type.error());
        }
        parameter.resolved_type = *parameter_type;
    }

    auto validated_dependency = validateReturnDependencies(*stored_decl);
    if (!validated_dependency) {
        return std::unexpected(validated_dependency.error());
    }

    if (stored_decl->impl_target_kind != ast::ImplTargetKind::None) {
        const auto* target_type = interfaceReceiverType(
            stored_decl->parameters.empty()
                ? nullptr
                : stored_decl->parameters.front().resolved_type);
        auto validated_impl =
            validateResolvedImplSignature(*stored_decl, target_type);
        if (!validated_impl) {
            return std::unexpected(validated_impl.error());
        }
        if (stored_decl->name == "drop") {
            types.registerDropFunction(target_type, stored_decl);
        }
    }

    auto analyzed = analyzeFunction(*stored_decl);
    if (!analyzed) {
        return std::unexpected(analyzed.error());
    }
    return stored_decl;
}

auto SemanticAnalyzer::ensureDropImplForType(const Type* type)
    -> std::expected<void, Diagnostic> {
    type = types.unqualify(type);
    if (types.dropFunction(type) != nullptr) {
        return {};
    }

    if ((type->kind != TypeKind::Struct || type->struct_decl == nullptr ||
         type->struct_decl->template_decl == nullptr) &&
        (type->kind != TypeKind::Enum || type->enum_decl == nullptr ||
         type->enum_decl->template_decl == nullptr)) {
        return {};
    }

    const auto impl_it = package_impls.find(
        make_impl_key("drop", impl_target_group_key(types, type)));
    if (impl_it == package_impls.end()) {
        return {};
    }
    for (auto* candidate : impl_it->second) {
        TypeBindings type_bindings;
        auto matched = matchTypePattern(
            *implReceiverPattern(*candidate), *candidate->owner_module,
            candidate->type_parameters, type, type_bindings);
        if (!matched) {
            continue;
        }
        if (candidate->type_parameters.empty()) {
            return {};
        }
        auto instantiated =
            instantiateFunctionTemplate(*candidate, type_bindings);
        if (!instantiated) {
            return std::unexpected(instantiated.error());
        }
        return {};
    }
    return {};
}

auto SemanticAnalyzer::resolveType(ast::TypeSyntax& type)
    -> std::expected<const Type*, Diagnostic> {
    if (type.resolved_type != nullptr) {
        return type.resolved_type;
    }

    if (type.is_shared && type.kind != ast::TypeSyntax::Kind::Pointer) {
        return unexpected_result<const Type*>(
            "shared qualifier is only supported on pointer types", type.range);
    }

    auto apply_const =
        [&](const Type* resolved) -> std::expected<const Type*, Diagnostic> {
        if (!type.is_const) {
            type.resolved_type = resolved;
            return type.resolved_type;
        }
        if (resolved->kind == TypeKind::Borrow ||
            resolved->kind == TypeKind::Interface) {
            return unexpected_result<const Type*>(
                "borrow and interface types do not support const qualifiers",
                type.range);
        }
        type.resolved_type = types.getConst(resolved);
        return type.resolved_type;
    };

    switch (type.kind) {
    case ast::TypeSyntax::Kind::Named: {
        if (type.type_arguments.empty()) {
            if (const auto* named = findVisibleNamedType(type.name);
                named != nullptr) {
                auto ensured_drop = ensureDropImplForType(named);
                if (!ensured_drop) {
                    return std::unexpected(ensured_drop.error());
                }
                return apply_const(named);
            }

            if (active_module == nullptr) {
                return unexpected_result<const Type*>(
                    "unknown type '" + type.name + "'", type.range);
            }
            if (findVisibleStructTemplate(type.name) != nullptr ||
                findVisibleEnumTemplate(type.name) != nullptr) {
                return unexpected_result<const Type*>(
                    "generic type '" + type.name +
                        "' requires explicit type arguments",
                    type.range);
            }
            return unexpected_result<const Type*>(
                "unknown type '" + type.name + "'", type.range);
        }

        std::vector<const Type*> type_arguments;
        type_arguments.reserve(type.type_arguments.size());
        for (auto& type_argument : type.type_arguments) {
            auto resolved_argument = resolveType(*type_argument);
            if (!resolved_argument) {
                return std::unexpected(resolved_argument.error());
            }
            type_arguments.push_back(*resolved_argument);
        }

        if (active_module == nullptr) {
            return unexpected_result<const Type*>(
                "type '" + type.name + "' is not generic", type.range);
        }
        if (auto* struct_decl = findVisibleStructTemplate(type.name);
            struct_decl != nullptr) {
            if (struct_decl->type_parameters.size() != type_arguments.size()) {
                return unexpected_result<const Type*>(
                    "wrong number of type arguments for '" + type.name + "'",
                    type.range);
            }
            auto instantiated =
                instantiateStructTemplate(*struct_decl, type_arguments);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            auto ensured_drop =
                ensureDropImplForType((*instantiated)->resolved_type);
            if (!ensured_drop) {
                return std::unexpected(ensured_drop.error());
            }
            return apply_const((*instantiated)->resolved_type);
        }

        if (auto* enum_decl = findVisibleEnumTemplate(type.name);
            enum_decl != nullptr) {
            if (enum_decl->type_parameters.size() != type_arguments.size()) {
                return unexpected_result<const Type*>(
                    "wrong number of type arguments for '" + type.name + "'",
                    type.range);
            }
            auto instantiated =
                instantiateEnumTemplate(*enum_decl, type_arguments);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            auto ensured_drop =
                ensureDropImplForType((*instantiated)->resolved_type);
            if (!ensured_drop) {
                return std::unexpected(ensured_drop.error());
            }
            return apply_const((*instantiated)->resolved_type);
        }

        return unexpected_result<const Type*>(
            "type '" + type.name + "' is not generic", type.range);
    }
    case ast::TypeSyntax::Kind::Borrow: {
        if (type.is_const) {
            return unexpected_result<const Type*>(
                "borrow types do not support const qualifiers", type.range);
        }
        if (type.element_type != nullptr &&
            type.element_type->kind == ast::TypeSyntax::Kind::Borrow) {
            return unexpected_result<const Type*>(
                "nested borrow types are not supported", type.range);
        }
        if (type.is_mut && type.element_type != nullptr &&
            type.element_type->kind == ast::TypeSyntax::Kind::Named &&
            (findVisibleInterface(type.element_type->name) != nullptr ||
             findVisibleInterfaceAlias(type.element_type->name) != nullptr)) {
            return unexpected_result<const Type*>(
                "interface borrows use the interface's declared receiver "
                "mutability; write '&fmt' instead of '&mut fmt'",
                type.range);
        }
        if (type.element_type != nullptr &&
            type.element_type->kind == ast::TypeSyntax::Kind::Named &&
            type.element_type->type_arguments.empty()) {
            if (type.element_type->is_const) {
                return unexpected_result<const Type*>(
                    "borrow pointee types do not support const qualifiers",
                    type.range);
            }
            if (const auto* interface_decl =
                    findVisibleInterface(type.element_type->name);
                interface_decl != nullptr) {
                type.resolved_type = types.getInterface(interface_decl);
                return type.resolved_type;
            }
            if (const auto* interface_alias_decl =
                    findVisibleInterfaceAlias(type.element_type->name);
                interface_alias_decl != nullptr) {
                if (interface_alias_decl->resolved_type == nullptr) {
                    return unexpected_result<const Type*>(
                        "unknown interface alias '" + type.element_type->name +
                            "'",
                        type.range);
                }
                type.resolved_type = interface_alias_decl->resolved_type;
                return type.resolved_type;
            }
        }
        auto element = resolveType(*type.element_type);
        if (!element) {
            return std::unexpected(element.error());
        }
        if (types.unqualify(*element) == types.voidType()) {
            return unexpected_result<const Type*>("cannot borrow void",
                                                  type.range);
        }
        if ((*element)->is_const) {
            return unexpected_result<const Type*>(
                "borrow pointee types do not support const qualifiers",
                type.range);
        }
        type.resolved_type = types.getBorrow(*element, type.is_mut);
        return type.resolved_type;
    }
    case ast::TypeSyntax::Kind::Slice: {
        auto element = resolveType(*type.element_type);
        if (!element) {
            return std::unexpected(element.error());
        }
        if (types.unqualify(*element) == types.voidType()) {
            return unexpected_result<const Type*>(
                "slice elements cannot have type void", type.range);
        }
        if (is_borrow_like_type(*element)) {
            if ((*element)->is_mut) {
                return unexpected_result<const Type*>(
                    "slice elements cannot have mutable borrow type",
                    type.range);
            }
            return apply_const(types.getSlice(*element));
        }
        if (types.unqualify(*element)->kind == TypeKind::Slice) {
            return unexpected_result<const Type*>(
                "slice elements cannot themselves be slice types", type.range);
        }
        if (typeContainsViews(*element)) {
            return unexpected_result<const Type*>(
                "slice elements cannot contain borrow, interface, or slice "
                "subobjects",
                type.range);
        }
        return apply_const(types.getSlice(*element));
    }
    case ast::TypeSyntax::Kind::Pointer: {
        auto element = resolveType(*type.element_type);
        if (!element) {
            return std::unexpected(element.error());
        }
        if (is_borrow_like_type(*element)) {
            return unexpected_result<const Type*>(
                "raw pointers to borrow or interface types are not supported",
                type.range);
        }
        return apply_const(types.getPointer(*element, type.is_shared));
    }
    case ast::TypeSyntax::Kind::Array: {
        auto element = resolveType(*type.element_type);
        if (!element) {
            return std::unexpected(element.error());
        }
        if (types.unqualify(*element) == types.voidType()) {
            return unexpected_result<const Type*>(
                "array elements cannot have type void", type.range);
        }
        if (typeContainsViews(*element) && !types.isCopy(*element)) {
            return unexpected_result<const Type*>(
                "array elements that contain borrow, interface, or slice "
                "subobjects must be copy",
                type.range);
        }
        return apply_const(types.getArray(*element, type.array_size));
    }
    }
    return unexpected_result<const Type*>("invalid type syntax", type.range);
}

auto SemanticAnalyzer::validateResolvedImplSignature(ast::FunctionDecl& decl,
                                                     const Type* target_type)
    -> std::expected<void, Diagnostic> {
    if (decl.parameters.empty()) {
        return make_error("impl must declare a receiver parameter", decl.range);
    }
    if (target_type == nullptr || target_type == types.voidType() ||
        target_type->kind == TypeKind::Borrow ||
        target_type->kind == TypeKind::Interface) {
        return make_error("impl receiver must be a concrete borrow",
                          decl.parameters.front().range);
    }

    if (decl.name == "drop") {
        if (decl.is_unchecked) {
            return make_error("impl drop cannot be declared unchecked",
                              decl.name_range);
        }
        if (target_type->kind != TypeKind::Struct &&
            target_type->kind != TypeKind::Enum) {
            return make_error("impl drop receiver must be a struct or enum "
                              "borrow",
                              decl.parameters.front().range);
        }
        if (types.unqualify(decl.resolved_return_type) != types.voidType()) {
            return make_error("impl drop must return void",
                              decl.return_type->range);
        }
        if (decl.parameters.size() != 1) {
            return make_error("impl drop must take exactly one parameter",
                              decl.range);
        }
        const auto* receiver_type = decl.parameters.front().resolved_type;
        if (receiver_type->kind != TypeKind::Borrow || !receiver_type->is_mut ||
            receiver_type->element_type != target_type) {
            return make_error(
                "impl drop must take a '&mut T' receiver matching its target "
                "type",
                decl.parameters.front().range);
        }
        return {};
    }

    const auto* interface_decl = decl.interface_decl;
    if (interface_decl == nullptr) {
        return make_error("missing interface metadata for impl '" + decl.name +
                              "'",
                          decl.range);
    }
    if (interface_decl->is_unchecked && !decl.is_unchecked) {
        return make_error("impl for unchecked interface '" +
                              interface_decl->name +
                              "' must be declared unchecked",
                          decl.name_range);
    }
    if (!interface_decl->is_unchecked && decl.is_unchecked) {
        return make_error("impl for safe interface '" + interface_decl->name +
                              "' must not be declared unchecked",
                          decl.name_range);
    }
    if (decl.resolved_return_type != interface_decl->resolved_return_type) {
        return make_error("impl return type must match interface '" +
                              interface_decl->name + "'",
                          decl.return_type->range);
    }
    if (decl.parameters.size() != interface_decl->parameters.size()) {
        return make_error("impl parameter count must match interface '" +
                              interface_decl->name + "'",
                          decl.range);
    }
    const auto* receiver_type = decl.parameters.front().resolved_type;
    if (receiver_type->kind != TypeKind::Borrow ||
        receiver_type->is_mut != interface_decl->receiver_is_mut ||
        receiver_type->element_type != target_type) {
        return make_error(
            "impl receiver must match its target type and interface mutability",
            decl.parameters.front().range);
    }
    for (std::size_t index = 1; index < decl.parameters.size(); ++index) {
        if (!types.isSame(decl.parameters[index].resolved_type,
                          interface_decl->parameters[index].resolved_type)) {
            return make_error("impl parameter types must match interface '" +
                                  interface_decl->name + "'",
                              decl.parameters[index].range);
        }
    }
    return {};
}

auto SemanticAnalyzer::interfaceReceiverType(const Type* argument_type) const
    -> const Type* {
    if (argument_type == nullptr) {
        return nullptr;
    }
    if (argument_type->kind == TypeKind::Interface) {
        return nullptr;
    }
    if (argument_type->kind == TypeKind::Borrow) {
        if (argument_type->element_type == nullptr) {
            return nullptr;
        }
        return types.unqualify(argument_type->element_type);
    }
    const auto* concrete_type = types.unqualify(argument_type);
    if (concrete_type == types.voidType()) {
        return nullptr;
    }
    return concrete_type;
}

auto SemanticAnalyzer::typeHasThreadProperty(const Type* type,
                                             ast::ThreadPropertyKind kind) const
    -> bool {
    if (type == nullptr) {
        return false;
    }

    const auto* nominal_type = types.unqualify(type);
    if ((nominal_type->kind != TypeKind::Struct ||
         nominal_type->struct_decl == nullptr) &&
        (nominal_type->kind != TypeKind::Enum ||
         nominal_type->enum_decl == nullptr)) {
        return false;
    }

    const auto property_it = package_property_impls.find(kind);
    if (property_it == package_property_impls.end()) {
        return false;
    }

    const ast::Module* actual_owner_module = nullptr;
    std::string_view actual_base_name;
    std::vector<const Type*> actual_type_arguments;
    if (nominal_type->kind == TypeKind::Struct) {
        const auto* actual_decl = nominal_type->struct_decl;
        const auto* canonical_decl = actual_decl->template_decl != nullptr
                                         ? actual_decl->template_decl
                                         : actual_decl;
        actual_owner_module = canonical_decl->owner_module;
        actual_base_name = canonical_decl->name;
        if (actual_decl->template_decl != nullptr) {
            actual_type_arguments = actual_decl->type_arguments;
        }
    } else {
        const auto* actual_decl = nominal_type->enum_decl;
        const auto* canonical_decl = actual_decl->template_decl != nullptr
                                         ? actual_decl->template_decl
                                         : actual_decl;
        actual_owner_module = canonical_decl->owner_module;
        actual_base_name = canonical_decl->name;
        if (actual_decl->template_decl != nullptr) {
            actual_type_arguments = actual_decl->type_arguments;
        }
    }

    for (const auto* property_decl : property_it->second) {
        if (property_decl == nullptr || property_decl->target_type == nullptr ||
            property_decl->impl_target_module != actual_owner_module ||
            property_decl->impl_target_name != actual_base_name) {
            continue;
        }

        if (property_decl->target_type->type_arguments.size() !=
            actual_type_arguments.size()) {
            continue;
        }

        std::unordered_map<std::string_view, const Type*> type_bindings;
        bool matches = true;
        for (std::size_t index = 0;
             index < property_decl->target_type->type_arguments.size();
             ++index) {
            const auto& pattern =
                *property_decl->target_type->type_arguments[index];
            if (pattern.kind != ast::TypeSyntax::Kind::Named ||
                !pattern.type_arguments.empty()) {
                matches = false;
                break;
            }

            const auto parameter_it =
                std::ranges::find(property_decl->type_parameters, pattern.name);
            if (parameter_it == property_decl->type_parameters.end()) {
                matches = false;
                break;
            }

            const auto binding_it = type_bindings.find(pattern.name);
            if (binding_it != type_bindings.end()) {
                if (binding_it->second != actual_type_arguments[index]) {
                    matches = false;
                    break;
                }
                continue;
            }
            type_bindings.emplace(pattern.name, actual_type_arguments[index]);
        }

        if (matches) {
            return true;
        }
    }

    return false;
}

auto SemanticAnalyzer::typeContainsLocalProperty(const Type* type) const
    -> bool {
    std::unordered_map<const Type*, bool> memo;
    std::unordered_set<const Type*> active;

    const auto recurse = [&](const Type* current, const auto& self) -> bool {
        if (current == nullptr) {
            return false;
        }

        current = types.unqualify(current);
        if (const auto memo_it = memo.find(current); memo_it != memo.end()) {
            return memo_it->second;
        }
        if (typeHasThreadProperty(current, ast::ThreadPropertyKind::Local)) {
            memo.emplace(current, true);
            return true;
        }
        if (!active.insert(current).second) {
            return false;
        }

        bool contains_local = false;
        switch (current->kind) {
        case TypeKind::Void:
        case TypeKind::Integer:
        case TypeKind::Float:
        case TypeKind::Char:
        case TypeKind::Bool:
        case TypeKind::Interface:
            contains_local = false;
            break;
        case TypeKind::Borrow:
        case TypeKind::Slice:
        case TypeKind::Array:
            contains_local = self(current->element_type, self);
            break;
        case TypeKind::Pointer:
            contains_local =
                current->is_shared && self(current->element_type, self);
            break;
        case TypeKind::Struct:
            contains_local = std::ranges::any_of(
                current->struct_decl->fields, [&](const auto& field) {
                    return self(field.resolved_type, self);
                });
            break;
        case TypeKind::Enum:
            contains_local = std::ranges::any_of(
                current->enum_decl->variants, [&](const auto& variant) {
                    return variant.resolved_type != nullptr &&
                           self(variant.resolved_type, self);
                });
            break;
        }

        active.erase(current);
        memo.emplace(current, contains_local);
        return contains_local;
    };

    return recurse(type, recurse);
}

auto SemanticAnalyzer::typeIsThreadShareSafe(const Type* type) const -> bool {
    std::unordered_map<const Type*, bool> memo;
    std::unordered_set<const Type*> active;

    const auto recurse = [&](const Type* current, const auto& self) -> bool {
        if (current == nullptr) {
            return false;
        }

        current = types.unqualify(current);
        if (const auto memo_it = memo.find(current); memo_it != memo.end()) {
            return memo_it->second;
        }
        if (typeHasThreadProperty(current, ast::ThreadPropertyKind::Share)) {
            memo.emplace(current, true);
            return true;
        }
        if (!active.insert(current).second) {
            return true;
        }

        bool share_safe = false;
        switch (current->kind) {
        case TypeKind::Void:
        case TypeKind::Integer:
        case TypeKind::Float:
        case TypeKind::Char:
        case TypeKind::Bool:
            share_safe = true;
            break;
        case TypeKind::Interface:
            share_safe = false;
            break;
        case TypeKind::Borrow:
        case TypeKind::Slice:
        case TypeKind::Array:
            share_safe = self(current->element_type, self);
            break;
        case TypeKind::Pointer:
            share_safe =
                current->is_shared && self(current->element_type, self);
            break;
        case TypeKind::Struct:
            share_safe = std::ranges::all_of(
                current->struct_decl->fields, [&](const auto& field) {
                    return self(field.resolved_type, self);
                });
            break;
        case TypeKind::Enum:
            share_safe = std::ranges::all_of(
                current->enum_decl->variants, [&](const auto& variant) {
                    return variant.resolved_type == nullptr ||
                           self(variant.resolved_type, self);
                });
            break;
        }

        active.erase(current);
        memo.emplace(current, share_safe);
        return share_safe;
    };

    return recurse(type, recurse);
}

auto SemanticAnalyzer::findImplForType(const ast::InterfaceDecl& interface_decl,
                                       const Type* receiver_type)
    -> std::expected<ast::FunctionDecl*, Diagnostic> {
    const auto* concrete_type = interfaceReceiverType(receiver_type);
    if (concrete_type == nullptr || concrete_type->kind == TypeKind::Borrow ||
        concrete_type->kind == TypeKind::Interface) {
        return unexpected_result<ast::FunctionDecl*>(
            "interface receiver must be a concrete value",
            interface_decl.range);
    }

    const auto impl_it = package_impls.find(
        make_impl_key(interface_decl.name, interface_decl.owner_module,
                      impl_target_group_key(types, concrete_type)));
    const auto concrete_name = types.describe(concrete_type);
    if (impl_it == package_impls.end()) {
        return unexpected_result<ast::FunctionDecl*>(
            "type '" + concrete_name + "' does not implement interface '" +
                interface_decl.name + "'",
            interface_decl.range);
    }

    ast::FunctionDecl* matched_impl = nullptr;
    for (auto* candidate : impl_it->second) {
        TypeBindings bindings;
        auto matched = matchTypePattern(
            *implReceiverPattern(*candidate), *candidate->owner_module,
            candidate->type_parameters, concrete_type, bindings);
        if (!matched) {
            continue;
        }

        ast::FunctionDecl* resolved_impl = candidate;
        if (!candidate->type_parameters.empty()) {
            auto instantiated =
                instantiateFunctionTemplate(*candidate, bindings);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            resolved_impl = *instantiated;
        }
        if (matched_impl != nullptr) {
            return unexpected_result<ast::FunctionDecl*>(
                "duplicate impl match for interface '" + interface_decl.name +
                    "' on '" + concrete_name + "'",
                candidate->range);
        }
        matched_impl = resolved_impl;
    }

    if (matched_impl == nullptr) {
        return unexpected_result<ast::FunctionDecl*>(
            "type '" + concrete_name + "' does not implement interface '" +
                interface_decl.name + "'",
            interface_decl.range);
    }
    return matched_impl;
}

auto SemanticAnalyzer::typeImplementsInterface(
    const ast::InterfaceDecl& interface_decl, const Type* receiver_type)
    -> bool {
    const auto* concrete_type = interfaceReceiverType(receiver_type);
    if (concrete_type == nullptr || concrete_type->kind == TypeKind::Borrow ||
        concrete_type->kind == TypeKind::Interface) {
        return false;
    }

    const auto impl_it = package_impls.find(
        make_impl_key(interface_decl.name, interface_decl.owner_module,
                      impl_target_group_key(types, concrete_type)));
    if (impl_it == package_impls.end()) {
        return false;
    }

    bool matched = false;
    for (auto* candidate : impl_it->second) {
        TypeBindings bindings;
        auto candidate_match = matchTypePattern(
            *implReceiverPattern(*candidate), *candidate->owner_module,
            candidate->type_parameters, concrete_type, bindings);
        if (!candidate_match) {
            continue;
        }
        if (matched) {
            return false;
        }
        matched = true;
    }
    return matched;
}

auto SemanticAnalyzer::typeSatisfiesInterfaceMarker(
    const Type* type, std::string_view marker) const -> bool {
    if (marker == "local") {
        return typeContainsLocalProperty(type);
    }
    if (marker == "share") {
        return typeIsThreadShareSafe(type);
    }
    if (marker == "send") {
        std::unordered_map<const Type*, bool> memo;
        std::unordered_set<const Type*> active;
        const auto recurse = [&](const Type* current,
                                 const auto& self) -> bool {
            if (current == nullptr) {
                return false;
            }

            current = types.unqualify(current);
            if (const auto memo_it = memo.find(current);
                memo_it != memo.end()) {
                return memo_it->second;
            }
            if (typeHasThreadProperty(current, ast::ThreadPropertyKind::Send)) {
                memo.emplace(current, true);
                return true;
            }
            if (!active.insert(current).second) {
                return true;
            }

            bool send_safe = false;
            switch (current->kind) {
            case TypeKind::Void:
            case TypeKind::Integer:
            case TypeKind::Float:
            case TypeKind::Char:
            case TypeKind::Bool:
                send_safe = true;
                break;
            case TypeKind::Interface:
            case TypeKind::Borrow:
            case TypeKind::Slice:
                send_safe = false;
                break;
            case TypeKind::Array:
                send_safe = self(current->element_type, self);
                break;
            case TypeKind::Pointer:
                send_safe = current->is_shared &&
                            typeIsThreadShareSafe(current->element_type);
                break;
            case TypeKind::Struct:
                send_safe = std::ranges::all_of(
                    current->struct_decl->fields, [&](const auto& field) {
                        return self(field.resolved_type, self);
                    });
                break;
            case TypeKind::Enum:
                send_safe = std::ranges::all_of(
                    current->enum_decl->variants, [&](const auto& variant) {
                        return variant.resolved_type == nullptr ||
                               self(variant.resolved_type, self);
                    });
                break;
            }

            active.erase(current);
            memo.emplace(current, send_safe);
            return send_safe;
        };
        return recurse(type, recurse);
    }
    return false;
}

auto SemanticAnalyzer::coerceExprToInterface(FunctionState& state,
                                             ast::Expr& expr,
                                             const Type* interface_type)
    -> std::expected<void, Diagnostic> {
    if (interface_type == nullptr ||
        interface_type->kind != TypeKind::Interface) {
        return make_error("invalid interface coercion target", expr.range);
    }

    expr.interface_impls.clear();
    expr.interface_source_type = nullptr;

    auto source_type = analyzeExpr(state, expr, nullptr);
    if (!source_type) {
        return std::unexpected(source_type.error());
    }
    if (types.sameIgnoringTopLevelConst(*source_type, interface_type)) {
        expr.resolved_type = interface_type;
        return {};
    }
    if ((*source_type)->kind == TypeKind::Interface) {
        const auto* source_interface_type = types.unqualify(*source_type);
        if (interface_type->is_mut && !source_interface_type->is_mut) {
            return unexpected_result<void>(
                "interface requires a mutable receiver borrow", expr.range);
        }

        const auto find_member_slot = [&](const ast::InterfaceDecl* member)
            -> std::optional<std::size_t> {
            for (std::size_t index = 0;
                 index < source_interface_type->interface_members.size();
                 ++index) {
                if (source_interface_type->interface_members[index] == member) {
                    return index;
                }
            }
            return std::nullopt;
        };

        std::optional<std::size_t> previous_slot;
        for (const auto* member : interface_type->interface_members) {
            if (member == nullptr) {
                continue;
            }
            const auto slot = find_member_slot(member);
            if (!slot.has_value()) {
                return unexpected_result<void>(
                    "interface value does not match expected interface type",
                    expr.range);
            }
            if (previous_slot.has_value() && *slot != *previous_slot + 1) {
                return unexpected_result<void>(
                    "interface value does not match expected interface type",
                    expr.range);
            }
            previous_slot = *slot;
        }

        for (const auto* excluded : interface_type->interface_exclusions) {
            if (excluded == nullptr) {
                continue;
            }
            if (std::ranges::find(source_interface_type->interface_exclusions,
                                  excluded) ==
                source_interface_type->interface_exclusions.end()) {
                return unexpected_result<void>(
                    "interface value does not guarantee excluded interface '" +
                        excluded->name + "'",
                    expr.range);
            }
        }
        for (const auto& marker : interface_type->interface_markers) {
            if (std::ranges::find(source_interface_type->interface_markers,
                                  marker) ==
                source_interface_type->interface_markers.end()) {
                return unexpected_result<void>(
                    "interface value does not guarantee marker '" + marker +
                        "'",
                    expr.range);
            }
        }
        for (const auto& marker : interface_type->interface_marker_exclusions) {
            if (std::ranges::find(
                    source_interface_type->interface_marker_exclusions,
                    marker) ==
                source_interface_type->interface_marker_exclusions.end()) {
                return unexpected_result<void>(
                    "interface value does not guarantee excluded marker '" +
                        marker + "'",
                    expr.range);
            }
        }

        expr.interface_source_type = *source_type;
        expr.resolved_type = interface_type;
        return {};
    }
    const auto* concrete_source_type = interfaceReceiverType(*source_type);
    if (concrete_source_type == nullptr) {
        return unexpected_result<void>(
            "interface values require a concrete place or borrow source",
            expr.range);
    }
    if ((*source_type)->kind == TypeKind::Borrow) {
        if (interface_type->is_mut && !(*source_type)->is_mut) {
            return unexpected_result<void>(
                "interface requires a mutable receiver borrow", expr.range);
        }
    } else {
        if (!expr.resolved_place.has_value()) {
            return unexpected_result<void>(
                "interface values require a concrete place or borrow source",
                expr.range);
        }
        if (interface_type->is_mut && (*source_type)->is_const) {
            return unexpected_result<void>(
                "interface requires a mutable receiver place", expr.range);
        }
    }

    std::vector<const ast::FunctionDecl*> interface_impls;
    interface_impls.reserve(interface_type->interface_members.size());
    for (const auto* member : interface_type->interface_members) {
        if (member == nullptr) {
            continue;
        }
        auto impl = findImplForType(*member, *source_type);
        if (!impl) {
            return std::unexpected(impl.error());
        }
        interface_impls.push_back(*impl);
    }
    for (const auto* excluded : interface_type->interface_exclusions) {
        if (excluded == nullptr) {
            continue;
        }
        if (typeImplementsInterface(*excluded, concrete_source_type)) {
            return unexpected_result<void>(
                "type '" + types.describe(concrete_source_type) +
                    "' matches excluded interface '" + excluded->name + "'",
                expr.range);
        }
    }
    for (const auto& marker : interface_type->interface_markers) {
        if (!typeSatisfiesInterfaceMarker(concrete_source_type, marker)) {
            return unexpected_result<void>(
                "type '" + types.describe(concrete_source_type) +
                    "' does not satisfy marker '" + marker + "'",
                expr.range);
        }
    }
    for (const auto& marker : interface_type->interface_marker_exclusions) {
        if (typeSatisfiesInterfaceMarker(concrete_source_type, marker)) {
            return unexpected_result<void>(
                "type '" + types.describe(concrete_source_type) +
                    "' matches excluded marker '" + marker + "'",
                expr.range);
        }
    }

    expr.interface_source_type = (*source_type)->kind == TypeKind::Borrow
                                     ? *source_type
                                     : concrete_source_type;
    expr.interface_impls = std::move(interface_impls);
    expr.resolved_type = interface_type;
    return {};
}

auto SemanticAnalyzer::coerceExprToSlice(FunctionState& state, ast::Expr& expr,
                                         const Type* slice_type)
    -> std::expected<void, Diagnostic> {
    if (slice_type == nullptr ||
        types.unqualify(slice_type)->kind != TypeKind::Slice) {
        return make_error("invalid slice coercion target", expr.range);
    }

    expr.slice_source_type = nullptr;
    expr.slice_source_place.reset();

    auto source_type = analyzeExpr(state, expr, nullptr);
    if (!source_type) {
        return std::unexpected(source_type.error());
    }
    if (*source_type == slice_type) {
        return {};
    }

    const auto* source_base = types.unqualify(*source_type);
    const auto* target_base = types.unqualify(slice_type);
    if (source_base->kind == TypeKind::Slice &&
        can_consume_value_type(types, *source_type, slice_type)) {
        return {};
    }

    if (source_base->kind == TypeKind::Array &&
        can_add_const_in_object_graph(types, source_base->element_type,
                                      target_base->element_type)) {
        if (expr.resolved_place.has_value()) {
            auto ignored_local = expr.resolved_place->owner_local_id;
            auto borrow_targets = projectedPlaceTargets(state, expr);
            if (!borrow_targets) {
                return std::unexpected(borrow_targets.error());
            }
            if (borrow_targets->empty()) {
                *borrow_targets =
                    std::vector<ast::ResolvedPlace>{*expr.resolved_place};
            }
            for (const auto& borrow_target : *borrow_targets) {
                auto borrow = ensureCanBorrow(state, borrow_target, false,
                                              expr.range, ignored_local);
                if (!borrow) {
                    return std::unexpected(borrow.error());
                }
            }
        }
        expr.slice_source_type = *source_type;
        if (expr.resolved_place.has_value()) {
            expr.slice_source_place = *expr.resolved_place;
        }
        expr.resolved_type = slice_type;
        return {};
    }

    if (source_base->kind == TypeKind::Borrow &&
        source_base->element_type != nullptr &&
        types.unqualify(source_base->element_type)->kind == TypeKind::Array &&
        can_add_const_in_object_graph(
            types, types.unqualify(source_base->element_type)->element_type,
            target_base->element_type)) {
        auto source_place = borrowSourcePlace(state, expr);
        if (!source_place) {
            return std::unexpected(source_place.error());
        }
        auto source_local_id = borrowSourceLocalId(state, expr);
        if (!source_local_id) {
            return std::unexpected(source_local_id.error());
        }
        auto ignored_local = source_local_id->has_value()
                                 ? std::optional<std::size_t>(*source_local_id)
                                 : std::nullopt;
        auto borrow = ensureCanBorrow(state, *source_place, false, expr.range,
                                      ignored_local);
        if (!borrow) {
            return std::unexpected(borrow.error());
        }
        expr.slice_source_type = *source_type;
        expr.slice_source_place = *source_place;
        if (source_local_id->has_value()) {
            expr.slice_source_place->owner_local_id = *source_local_id;
        }
        expr.resolved_type = slice_type;
        return {};
    }

    return unexpected_result<void>(
        "slice values require an array, array borrow, or compatible slice "
        "source",
        expr.range);
}

auto SemanticAnalyzer::sliceSourcePlace(FunctionState& state, ast::Expr& expr)
    -> std::expected<std::optional<ast::ResolvedPlace>, Diagnostic> {
    if (expr.cached_view_bindings.has_value()) {
        const auto it =
            std::ranges::find_if(*expr.cached_view_bindings,
                                 [](const ast::CachedViewBinding& binding) {
                                     return binding.path.empty();
                                 });
        if (it != expr.cached_view_bindings->end()) {
            if (!it->source_places.empty()) {
                auto source_place = placeSetRepresentative(it->source_places);
                if (source_place.has_value() &&
                    it->source_local_id.has_value()) {
                    source_place->owner_local_id = *it->source_local_id;
                }
                return source_place;
            }
            return std::optional<ast::ResolvedPlace>{};
        }
    }

    if (expr.slice_source_place.has_value()) {
        return expr.slice_source_place;
    }

    const Type* expr_type = expr.resolved_type;
    if (expr_type == nullptr) {
        auto analyzed = analyzeExpr(state, expr);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        expr_type = *analyzed;
    }
    if (expr.slice_source_place.has_value()) {
        return expr.slice_source_place;
    }
    if (types.unqualify(expr_type)->kind != TypeKind::Slice) {
        return std::optional<ast::ResolvedPlace>{};
    }

    if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr && unary->op == ast::UnaryOp::Move) {
        return sliceSourcePlace(state, *unary->operand);
    }

    if (auto* name = std::get_if<ast::NameExpr>(&expr.node); name != nullptr) {
        const auto local_index = findLocalById(state, name->local_id);
        if (local_index.has_value()) {
            auto source_place = placeSetRepresentative(
                topLevelOrigins(state.locals[*local_index]));
            if (source_place.has_value()) {
                source_place->owner_local_id =
                    state.locals[*local_index].unique_id;
            }
            return source_place;
        }
    }

    if (expr.resolved_place.has_value()) {
        if (const auto slot_index = findViewSlotLocal(
                state, expr.resolved_place->is_external,
                expr.resolved_place->root_id, expr.resolved_place->fields);
            slot_index.has_value()) {
            auto source_place = placeSetRepresentative(
                topLevelOrigins(state.locals[*slot_index]));
            if (source_place.has_value()) {
                source_place->owner_local_id =
                    state.locals[*slot_index].unique_id;
            }
            return source_place;
        }
    }

    return std::optional<ast::ResolvedPlace>{};
}

auto SemanticAnalyzer::matchTypePattern(
    const ast::TypeSyntax& pattern, const ast::Module& owner_module,
    const std::vector<std::string>& type_parameters, const Type* actual_type,
    TypeBindings& type_bindings) -> std::expected<void, Diagnostic> {
    if (actual_type == nullptr) {
        return make_error("generic argument does not match parameter type",
                          pattern.range);
    }

    switch (pattern.kind) {
    case ast::TypeSyntax::Kind::Named: {
        if (pattern.type_arguments.empty() &&
            std::ranges::find(type_parameters, pattern.name) !=
                type_parameters.end()) {
            if (const auto binding_it = type_bindings.find(pattern.name);
                binding_it != type_bindings.end()) {
                if (binding_it->second != actual_type) {
                    return make_error(
                        "generic argument inference produced conflicting "
                        "types for '" +
                            pattern.name + "'",
                        pattern.range);
                }
            } else {
                type_bindings.emplace(pattern.name, actual_type);
            }
            return {};
        }

        if (pattern.type_arguments.empty()) {
            const auto* named_type =
                findNamedTypeInModule(owner_module, pattern.name);
            if (named_type != nullptr && pattern.is_const) {
                named_type = types.getConst(named_type);
            }
            if (named_type == nullptr || named_type != actual_type) {
                return make_error(
                    "generic argument does not match parameter type",
                    pattern.range);
            }
            return {};
        }

        std::string expected_base_name;
        if (const auto* struct_decl =
                findStructTemplateInModule(owner_module, pattern.name);
            struct_decl != nullptr) {
            expected_base_name = struct_decl->linkage_name;
        } else if (const auto* enum_decl =
                       findEnumTemplateInModule(owner_module, pattern.name);
                   enum_decl != nullptr) {
            expected_base_name = enum_decl->linkage_name;
        } else {
            return make_error("generic argument does not match parameter type",
                              pattern.range);
        }

        std::string actual_base_name;
        std::vector<const Type*> actual_type_arguments;
        if (actual_type->kind == TypeKind::Struct &&
            actual_type->struct_decl != nullptr) {
            actual_base_name =
                actual_type->struct_decl->template_decl != nullptr
                    ? actual_type->struct_decl->template_decl->linkage_name
                    : actual_type->struct_decl->linkage_name;
            actual_type_arguments = actual_type->struct_decl->type_arguments;
        } else if (actual_type->kind == TypeKind::Enum &&
                   actual_type->enum_decl != nullptr) {
            actual_base_name =
                actual_type->enum_decl->template_decl != nullptr
                    ? actual_type->enum_decl->template_decl->linkage_name
                    : actual_type->enum_decl->linkage_name;
            actual_type_arguments = actual_type->enum_decl->type_arguments;
        } else {
            return make_error("generic argument does not match parameter type",
                              pattern.range);
        }

        if (actual_base_name != expected_base_name ||
            actual_type_arguments.size() != pattern.type_arguments.size()) {
            return make_error("generic argument does not match parameter type",
                              pattern.range);
        }

        for (std::size_t index = 0; index < pattern.type_arguments.size();
             ++index) {
            auto matched = matchTypePattern(
                *pattern.type_arguments[index], owner_module, type_parameters,
                actual_type_arguments[index], type_bindings);
            if (!matched) {
                return std::unexpected(matched.error());
            }
        }
        return {};
    }
    case ast::TypeSyntax::Kind::Borrow: {
        if (actual_type->kind != TypeKind::Borrow ||
            (pattern.is_mut && !actual_type->is_mut)) {
            return make_error("generic argument does not match parameter type",
                              pattern.range);
        }
        return matchTypePattern(*pattern.element_type, owner_module,
                                type_parameters, actual_type->element_type,
                                type_bindings);
    }
    case ast::TypeSyntax::Kind::Slice: {
        const auto* actual_base = types.unqualify(actual_type);
        if (actual_base->kind == TypeKind::Slice) {
            return matchTypePattern(*pattern.element_type, owner_module,
                                    type_parameters, actual_base->element_type,
                                    type_bindings);
        }
        if (actual_base->kind == TypeKind::Array) {
            return matchTypePattern(*pattern.element_type, owner_module,
                                    type_parameters, actual_base->element_type,
                                    type_bindings);
        }
        return make_error("generic argument does not match parameter type",
                          pattern.range);
    }
    case ast::TypeSyntax::Kind::Pointer: {
        if (actual_type->kind != TypeKind::Pointer ||
            actual_type->is_shared != pattern.is_shared) {
            return make_error("generic argument does not match parameter type",
                              pattern.range);
        }
        return matchTypePattern(*pattern.element_type, owner_module,
                                type_parameters, actual_type->element_type,
                                type_bindings);
    }
    case ast::TypeSyntax::Kind::Array: {
        if (actual_type->kind != TypeKind::Array ||
            actual_type->array_size != pattern.array_size) {
            return make_error("generic argument does not match parameter type",
                              pattern.range);
        }
        return matchTypePattern(*pattern.element_type, owner_module,
                                type_parameters, actual_type->element_type,
                                type_bindings);
    }
    }
    return make_error("invalid generic type pattern", pattern.range);
}

auto SemanticAnalyzer::inferTypeBindings(
    FunctionState& state, const ast::FunctionDecl& decl,
    const std::vector<ast::Expr*>& arguments)
    -> std::expected<TypeBindings, Diagnostic> {
    if (arguments.size() != decl.parameters.size()) {
        return unexpected_result<TypeBindings>(
            "call argument count does not match function signature",
            decl.range);
    }

    TypeBindings type_bindings;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto* argument = arguments[index];
        const Type* expected_argument_type = nullptr;
        auto instantiated_parameter_type =
            clone_type_syntax(*decl.parameters[index].type, type_bindings);
        if (!type_syntax_mentions_parameters(*instantiated_parameter_type,
                                             decl.type_parameters)) {
            auto resolved_parameter_type =
                resolveType(*instantiated_parameter_type);
            if (!resolved_parameter_type) {
                return std::unexpected(resolved_parameter_type.error());
            }
            expected_argument_type = *resolved_parameter_type;
        }

        auto argument_type =
            analyzeExpr(state, *argument, expected_argument_type);
        if (!argument_type) {
            return std::unexpected(argument_type.error());
        }
        auto matched = matchTypePattern(
            *decl.parameters[index].type, *decl.owner_module,
            decl.type_parameters, *argument_type, type_bindings);
        if (!matched) {
            return std::unexpected(matched.error());
        }
    }

    for (const auto& type_parameter : decl.type_parameters) {
        if (!type_bindings.contains(type_parameter)) {
            return unexpected_result<TypeBindings>(
                "could not infer type argument for '" + type_parameter + "'",
                decl.range);
        }
    }
    return type_bindings;
}

} // namespace cyan

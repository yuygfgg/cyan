#include "sc/lsp_support.hpp"

#include "sc/lexer.hpp"
#include "sc/parser.hpp"

#include <glaze/core/common.hpp>
#include <glaze/json/write.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace sc {

namespace {

template <typename... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

struct TypeParameterInfo {
    std::string name;
    SourceRange declaration_range;
};

auto contains_offset(SourceRange range, std::size_t offset) -> bool {
    if (range.source == nullptr || range.end <= range.begin) {
        return false;
    }
    return range.begin <= offset && offset < range.end;
}

auto range_size(SourceRange range) -> std::size_t {
    return range.end >= range.begin ? range.end - range.begin : 0;
}

auto better_match(SourceRange candidate, SourceRange current) -> bool {
    if (current.source == nullptr) {
        return true;
    }
    return range_size(candidate) <= range_size(current);
}

auto kind_for_local_symbol(SemanticLocalSymbol::Kind kind) -> LSPSymbolKind {
    switch (kind) {
    case SemanticLocalSymbol::Kind::Var:
        return LSPSymbolKind::Local;
    case SemanticLocalSymbol::Kind::Parameter:
        return LSPSymbolKind::Parameter;
    case SemanticLocalSymbol::Kind::SwitchBinding:
        return LSPSymbolKind::SwitchBinding;
    }
    return LSPSymbolKind::Local;
}

auto canonical_function_decl(const ast::FunctionDecl* function)
    -> const ast::FunctionDecl* {
    if (function == nullptr) {
        return nullptr;
    }
    return function->template_decl != nullptr ? function->template_decl
                                              : function;
}

auto canonical_struct_decl(const ast::StructDecl* decl)
    -> const ast::StructDecl* {
    if (decl == nullptr) {
        return nullptr;
    }
    return decl->template_decl != nullptr ? decl->template_decl : decl;
}

auto canonical_enum_decl(const ast::EnumDecl* decl) -> const ast::EnumDecl* {
    if (decl == nullptr) {
        return nullptr;
    }
    return decl->template_decl != nullptr ? decl->template_decl : decl;
}

auto canonical_struct_field(const ast::StructDecl* decl, std::size_t index)
    -> const ast::StructField* {
    const auto* canonical = canonical_struct_decl(decl);
    if (canonical == nullptr || index >= canonical->fields.size()) {
        return nullptr;
    }
    return &canonical->fields[index];
}

auto canonical_enum_variant(const ast::EnumDecl* decl, std::size_t index)
    -> const ast::EnumVariant* {
    const auto* canonical = canonical_enum_decl(decl);
    if (canonical == nullptr || index >= canonical->variants.size()) {
        return nullptr;
    }
    return &canonical->variants[index];
}

auto slice_name(SourceRange range) -> std::string {
    if (range.source == nullptr) {
        return {};
    }
    return std::string(range.source->slice(range));
}

auto top_level_decl_range(const ast::Decl& decl) -> SourceRange {
    return std::visit([](const auto& inner) { return inner.range; }, decl);
}

class ModuleTraversal {
  public:
    ModuleTraversal(const ast::Module& module, const SemanticAnalysis& analysis,
                    std::vector<LSPSymbolOccurrence>* symbols,
                    std::optional<std::size_t> query_offset,
                    LSPQueryResult* query_result)
        : module(module), analysis(analysis), symbols(symbols),
          query_offset(query_offset), query_result(query_result) {
        if (this->query_result != nullptr) {
            this->query_result->module = &module;
        }
    }

    auto traverse() -> void {
        for (const auto& import_decl : module.imports) {
            visitImport(import_decl);
        }
        for (const auto& decl : module.declarations) {
            visitDecl(decl);
        }
    }

  private:
    auto emit(LSPSymbolOccurrence occurrence) -> void {
        if (symbols != nullptr) {
            symbols->push_back(occurrence);
        }
        if (query_offset.has_value() && query_result != nullptr &&
            contains_offset(occurrence.range, *query_offset) &&
            better_match(occurrence.range, best_symbol_range)) {
            best_symbol_range = occurrence.range;
            query_result->symbol = std::move(occurrence);
        }
    }

    auto noteStmt(const ast::Stmt& stmt) -> void {
        if (!query_offset.has_value() || query_result == nullptr ||
            !contains_offset(stmt.range, *query_offset)) {
            return;
        }
        if (query_result->statement == nullptr ||
            better_match(stmt.range, query_result->statement->range)) {
            query_result->statement = &stmt;
        }
    }

    auto noteExpr(const ast::Expr& expr) -> void {
        if (!query_offset.has_value() || query_result == nullptr ||
            !contains_offset(expr.range, *query_offset)) {
            return;
        }
        if (query_result->expression == nullptr ||
            better_match(expr.range, query_result->expression->range)) {
            query_result->expression = &expr;
        }
    }

    auto noteType(const ast::TypeSyntax& type) -> void {
        if (!query_offset.has_value() || query_result == nullptr ||
            !contains_offset(type.range, *query_offset)) {
            return;
        }
        if (query_result->type_syntax == nullptr ||
            better_match(type.range, query_result->type_syntax->range)) {
            query_result->type_syntax = &type;
        }
    }

    [[nodiscard]] auto
    findTypeParameter(const std::vector<TypeParameterInfo>& parameters,
                      std::string_view name) const -> const TypeParameterInfo* {
        const auto it = std::find_if(parameters.begin(), parameters.end(),
                                     [&](const TypeParameterInfo& parameter) {
                                         return parameter.name == name;
                                     });
        return it == parameters.end() ? nullptr : &*it;
    }

    [[nodiscard]] auto builtinTypeOccurrence(const ast::TypeSyntax& type) const
        -> LSPSymbolOccurrence {
        return LSPSymbolOccurrence{
            .kind = LSPSymbolKind::BuiltinType,
            .role = LSPSymbolRole::Reference,
            .name = type.name,
            .range = type.name_range,
            .declaration_range = type.name_range,
            .type = type.resolved_type,
        };
    }

    auto typeOccurrence(const ast::TypeSyntax& type,
                        const std::vector<TypeParameterInfo>& type_parameters)
        -> std::optional<LSPSymbolOccurrence> {
        if (type.kind != ast::TypeSyntax::Kind::Named ||
            type.name_range.source == nullptr) {
            return std::nullopt;
        }

        if (type.type_arguments.empty()) {
            if (const auto* parameter =
                    findTypeParameter(type_parameters, type.name);
                parameter != nullptr) {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::TypeParameter,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = parameter->declaration_range,
                    .type = type.resolved_type,
                };
            }
        }

        if (type.resolved_type == nullptr) {
            if (type.name == "void" || type.name == "int" ||
                type.name == "float" || type.name == "char" ||
                type.name == "bool") {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::BuiltinType,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = std::nullopt,
                };
            }
            const auto* scope = analysis.visibleScopeFor(module);
            if (scope == nullptr) {
                return std::nullopt;
            }
            if (const auto struct_it = scope->structs.find(type.name);
                struct_it != scope->structs.end()) {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::Struct,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = struct_it->second->name_range,
                };
            }
            if (const auto struct_it = scope->struct_templates.find(type.name);
                struct_it != scope->struct_templates.end()) {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::Struct,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = struct_it->second->name_range,
                };
            }
            if (const auto enum_it = scope->enums.find(type.name);
                enum_it != scope->enums.end()) {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::Enum,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = enum_it->second->name_range,
                };
            }
            if (const auto enum_it = scope->enum_templates.find(type.name);
                enum_it != scope->enum_templates.end()) {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::Enum,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = enum_it->second->name_range,
                };
            }
            if (const auto interface_it = scope->interfaces.find(type.name);
                interface_it != scope->interfaces.end()) {
                return LSPSymbolOccurrence{
                    .kind = LSPSymbolKind::Interface,
                    .role = LSPSymbolRole::Reference,
                    .name = type.name,
                    .range = type.name_range,
                    .declaration_range = interface_it->second->name_range,
                };
            }
            return std::nullopt;
        }

        switch (type.resolved_type->kind) {
        case TypeKind::Void:
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::Char:
        case TypeKind::Bool:
            return builtinTypeOccurrence(type);
        case TypeKind::Slice:
            return std::nullopt;
        case TypeKind::Struct: {
            const auto* struct_decl =
                canonical_struct_decl(type.resolved_type->struct_decl);
            if (struct_decl == nullptr) {
                return std::nullopt;
            }
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Struct,
                .role = LSPSymbolRole::Reference,
                .name = type.name,
                .range = type.name_range,
                .declaration_range = struct_decl->name_range,
                .type = type.resolved_type,
            };
        }
        case TypeKind::Enum: {
            const auto* enum_decl =
                canonical_enum_decl(type.resolved_type->enum_decl);
            if (enum_decl == nullptr) {
                return std::nullopt;
            }
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Enum,
                .role = LSPSymbolRole::Reference,
                .name = type.name,
                .range = type.name_range,
                .declaration_range = enum_decl->name_range,
                .type = type.resolved_type,
            };
        }
        case TypeKind::Interface:
            if (type.resolved_type->interface_decl == nullptr) {
                return std::nullopt;
            }
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Interface,
                .role = LSPSymbolRole::Reference,
                .name = type.name,
                .range = type.name_range,
                .declaration_range =
                    type.resolved_type->interface_decl->name_range,
                .type = type.resolved_type,
            };
        case TypeKind::Borrow:
        case TypeKind::Pointer:
        case TypeKind::Array:
            return std::nullopt;
        }
        return std::nullopt;
    }

    auto parameterReferenceOccurrence(const ast::FunctionDecl& function)
        -> std::vector<LSPSymbolOccurrence> {
        std::vector<LSPSymbolOccurrence> occurrences;
        for (const auto& dependency : function.return_dependencies) {
            for (const auto* path : {&dependency.source, &dependency.target}) {
                if (path->is_return) {
                    continue;
                }
                if (path->parameter_index.has_value() &&
                    *path->parameter_index < function.parameters.size()) {
                    const auto& parameter =
                        function.parameters[*path->parameter_index];
                    occurrences.push_back(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Parameter,
                        .role = LSPSymbolRole::Reference,
                        .name = parameter.name,
                        .range = path->root_range,
                        .declaration_range = parameter.name_range,
                        .type = parameter.resolved_type,
                    });
                } else if (!path->root_name.empty()) {
                    occurrences.push_back(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Parameter,
                        .role = LSPSymbolRole::Reference,
                        .name = path->root_name,
                        .range = path->root_range,
                        .declaration_range = std::nullopt,
                    });
                }
            }
        }
        return occurrences;
    }

    [[nodiscard]] auto nameOccurrence(const ast::NameExpr& name) const
        -> std::optional<LSPSymbolOccurrence> {
        if (name.name_range.source == nullptr) {
            return std::nullopt;
        }

        if (name.local_id != 0) {
            if (const auto* symbol = analysis.localSymbol(name.local_id);
                symbol != nullptr) {
                return LSPSymbolOccurrence{
                    .kind = kind_for_local_symbol(symbol->kind),
                    .role = LSPSymbolRole::Reference,
                    .name = name.name,
                    .range = name.name_range,
                    .declaration_range = symbol->name_range,
                    .type = symbol->type,
                };
            }
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Local,
                .role = LSPSymbolRole::Reference,
                .name = name.name,
                .range = name.name_range,
                .declaration_range = std::nullopt,
            };
        }

        const auto* function = canonical_function_decl(name.function);
        if (function != nullptr) {
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Function,
                .role = LSPSymbolRole::Reference,
                .name = name.name,
                .range = name.name_range,
                .declaration_range = function->name_range,
                .type = function->resolved_return_type,
            };
        }

        return std::nullopt;
    }

    [[nodiscard]] auto callOccurrence(const ast::CallExpr& call) const
        -> std::optional<LSPSymbolOccurrence> {
        if (call.callee_range.source == nullptr) {
            return std::nullopt;
        }

        if (call.enum_decl != nullptr) {
            const auto* variant =
                canonical_enum_variant(call.enum_decl, call.variant_index);
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Variant,
                .role = LSPSymbolRole::Reference,
                .name = call.callee,
                .range = call.callee_range,
                .declaration_range =
                    variant != nullptr
                        ? std::optional<SourceRange>(variant->name_range)
                        : std::nullopt,
                .type = call.enum_decl->resolved_type,
            };
        }

        if (call.dispatched_interface != nullptr) {
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Interface,
                .role = LSPSymbolRole::Reference,
                .name = call.callee,
                .range = call.callee_range,
                .declaration_range = call.dispatched_interface->name_range,
                .type = call.dispatched_interface->resolved_return_type,
            };
        }

        if (call.function == nullptr) {
            return std::nullopt;
        }

        if (call.function->interface_decl != nullptr &&
            call.function->impl_target_kind != ast::ImplTargetKind::None) {
            return LSPSymbolOccurrence{
                .kind = LSPSymbolKind::Interface,
                .role = LSPSymbolRole::Reference,
                .name = call.callee,
                .range = call.callee_range,
                .declaration_range = call.function->interface_decl->name_range,
                .type = call.function->interface_decl->resolved_return_type,
            };
        }

        const auto* function = canonical_function_decl(call.function);
        return LSPSymbolOccurrence{
            .kind = LSPSymbolKind::Function,
            .role = LSPSymbolRole::Reference,
            .name = call.callee,
            .range = call.callee_range,
            .declaration_range =
                function != nullptr
                    ? std::optional<SourceRange>(function->name_range)
                    : std::nullopt,
            .type = call.function->resolved_return_type,
        };
    }

    [[nodiscard]] auto memberOccurrence(const ast::MemberExpr& member) const
        -> std::optional<LSPSymbolOccurrence> {
        if (member.field_range.source == nullptr || member.base == nullptr ||
            member.base->resolved_type == nullptr) {
            return std::nullopt;
        }

        const Type* base_type = member.base->resolved_type;
        if (base_type->kind == TypeKind::Borrow) {
            base_type = base_type->element_type;
        }
        if (base_type == nullptr || base_type->kind != TypeKind::Struct) {
            return std::nullopt;
        }

        const auto* field =
            canonical_struct_field(base_type->struct_decl, member.field_index);
        return LSPSymbolOccurrence{
            .kind = LSPSymbolKind::Field,
            .role = LSPSymbolRole::Reference,
            .name = member.field_name,
            .range = member.field_range,
            .declaration_range =
                field != nullptr ? std::optional<SourceRange>(field->name_range)
                                 : std::nullopt,
            .type = field != nullptr ? field->resolved_type : nullptr,
        };
    }

    [[nodiscard]] auto
    variantOccurrence(const ast::SwitchCase& switch_case,
                      const ast::SwitchStmt& switch_stmt) const
        -> std::optional<LSPSymbolOccurrence> {
        if (switch_case.is_default ||
            switch_case.variant_name_range.source == nullptr ||
            switch_stmt.enum_decl == nullptr) {
            return std::nullopt;
        }
        const auto* variant = canonical_enum_variant(switch_stmt.enum_decl,
                                                     switch_case.variant_index);
        return LSPSymbolOccurrence{
            .kind = LSPSymbolKind::Variant,
            .role = LSPSymbolRole::Reference,
            .name = switch_case.variant_name,
            .range = switch_case.variant_name_range,
            .declaration_range =
                variant != nullptr
                    ? std::optional<SourceRange>(variant->name_range)
                    : std::nullopt,
            .type = switch_case.binding_type,
        };
    }

    auto visitImport(const ast::ImportDecl& import_decl) -> void {
        for (const auto& part_range : import_decl.module_name_part_ranges) {
            emit(LSPSymbolOccurrence{
                .kind = LSPSymbolKind::ImportModule,
                .role = LSPSymbolRole::Reference,
                .name = slice_name(part_range),
                .range = part_range,
                .declaration_range = std::nullopt,
            });
        }
    }

    auto visitDecl(const ast::Decl& decl) -> void {
        if (query_offset.has_value() && query_result != nullptr &&
            contains_offset(top_level_decl_range(decl), *query_offset)) {
            query_result->top_level_decl = &decl;
        }

        std::visit(
            Overloaded{
                [&](const ast::StructDecl& struct_decl) {
                    std::vector<TypeParameterInfo> type_parameters;
                    type_parameters.reserve(struct_decl.type_parameters.size());
                    for (std::size_t index = 0;
                         index < struct_decl.type_parameters.size() &&
                         index < struct_decl.type_parameter_ranges.size();
                         ++index) {
                        type_parameters.push_back(TypeParameterInfo{
                            .name = struct_decl.type_parameters[index],
                            .declaration_range =
                                struct_decl.type_parameter_ranges[index]});
                    }

                    emit(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Struct,
                        .role = LSPSymbolRole::Declaration,
                        .name = struct_decl.name,
                        .range = struct_decl.name_range,
                        .declaration_range = struct_decl.name_range,
                        .type = struct_decl.resolved_type,
                    });
                    for (std::size_t index = 0;
                         index < struct_decl.type_parameters.size() &&
                         index < struct_decl.type_parameter_ranges.size();
                         ++index) {
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::TypeParameter,
                            .role = LSPSymbolRole::Declaration,
                            .name = struct_decl.type_parameters[index],
                            .range = struct_decl.type_parameter_ranges[index],
                            .declaration_range =
                                struct_decl.type_parameter_ranges[index],
                        });
                    }
                    for (const auto& field : struct_decl.fields) {
                        visitType(*field.type, type_parameters);
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::Field,
                            .role = LSPSymbolRole::Declaration,
                            .name = field.name,
                            .range = field.name_range,
                            .declaration_range = field.name_range,
                            .type = field.resolved_type,
                        });
                    }
                },
                [&](const ast::EnumDecl& enum_decl) {
                    std::vector<TypeParameterInfo> type_parameters;
                    type_parameters.reserve(enum_decl.type_parameters.size());
                    for (std::size_t index = 0;
                         index < enum_decl.type_parameters.size() &&
                         index < enum_decl.type_parameter_ranges.size();
                         ++index) {
                        type_parameters.push_back(TypeParameterInfo{
                            .name = enum_decl.type_parameters[index],
                            .declaration_range =
                                enum_decl.type_parameter_ranges[index]});
                    }

                    emit(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Enum,
                        .role = LSPSymbolRole::Declaration,
                        .name = enum_decl.name,
                        .range = enum_decl.name_range,
                        .declaration_range = enum_decl.name_range,
                        .type = enum_decl.resolved_type,
                    });
                    for (std::size_t index = 0;
                         index < enum_decl.type_parameters.size() &&
                         index < enum_decl.type_parameter_ranges.size();
                         ++index) {
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::TypeParameter,
                            .role = LSPSymbolRole::Declaration,
                            .name = enum_decl.type_parameters[index],
                            .range = enum_decl.type_parameter_ranges[index],
                            .declaration_range =
                                enum_decl.type_parameter_ranges[index],
                        });
                    }
                    for (const auto& variant : enum_decl.variants) {
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::Variant,
                            .role = LSPSymbolRole::Declaration,
                            .name = variant.name,
                            .range = variant.name_range,
                            .declaration_range = variant.name_range,
                            .type = variant.resolved_type,
                        });
                        if (variant.payload_type != nullptr) {
                            visitType(*variant.payload_type, type_parameters);
                        }
                    }
                },
                [&](const ast::InterfaceDecl& interface_decl) {
                    std::vector<TypeParameterInfo> type_parameters;
                    if (interface_decl.receiver_type_parameter_range.source !=
                        nullptr) {
                        type_parameters.push_back(TypeParameterInfo{
                            .name = interface_decl.receiver_type_parameter,
                            .declaration_range =
                                interface_decl.receiver_type_parameter_range});
                    }

                    emit(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Interface,
                        .role = LSPSymbolRole::Declaration,
                        .name = interface_decl.name,
                        .range = interface_decl.name_range,
                        .declaration_range = interface_decl.name_range,
                    });
                    if (interface_decl.receiver_type_parameter_range.source !=
                        nullptr) {
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::TypeParameter,
                            .role = LSPSymbolRole::Declaration,
                            .name = interface_decl.receiver_type_parameter,
                            .range =
                                interface_decl.receiver_type_parameter_range,
                            .declaration_range =
                                interface_decl.receiver_type_parameter_range,
                        });
                    }
                    if (interface_decl.return_type != nullptr) {
                        visitType(*interface_decl.return_type, type_parameters);
                    }
                    for (const auto& parameter : interface_decl.parameters) {
                        visitType(*parameter.type, type_parameters);
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::Parameter,
                            .role = LSPSymbolRole::Declaration,
                            .name = parameter.name,
                            .range = parameter.name_range,
                            .declaration_range = parameter.name_range,
                            .type = parameter.resolved_type,
                        });
                    }
                },
                [&](const ast::FunctionDecl& function_decl) {
                    std::vector<TypeParameterInfo> type_parameters;
                    type_parameters.reserve(
                        function_decl.type_parameters.size());
                    for (std::size_t index = 0;
                         index < function_decl.type_parameters.size() &&
                         index < function_decl.type_parameter_ranges.size();
                         ++index) {
                        type_parameters.push_back(TypeParameterInfo{
                            .name = function_decl.type_parameters[index],
                            .declaration_range =
                                function_decl.type_parameter_ranges[index]});
                    }

                    if (query_result != nullptr && query_offset.has_value() &&
                        contains_offset(function_decl.range, *query_offset)) {
                        query_result->enclosing_function = &function_decl;
                    }

                    emit(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Function,
                        .role = LSPSymbolRole::Declaration,
                        .name = function_decl.name,
                        .range = function_decl.name_range,
                        .declaration_range = function_decl.name_range,
                        .type = function_decl.resolved_return_type,
                    });
                    for (std::size_t index = 0;
                         index < function_decl.type_parameters.size() &&
                         index < function_decl.type_parameter_ranges.size();
                         ++index) {
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::TypeParameter,
                            .role = LSPSymbolRole::Declaration,
                            .name = function_decl.type_parameters[index],
                            .range = function_decl.type_parameter_ranges[index],
                            .declaration_range =
                                function_decl.type_parameter_ranges[index],
                        });
                    }
                    if (function_decl.return_type != nullptr) {
                        visitType(*function_decl.return_type, type_parameters);
                    }
                    for (const auto& parameter : function_decl.parameters) {
                        visitType(*parameter.type, type_parameters);
                        emit(LSPSymbolOccurrence{
                            .kind = LSPSymbolKind::Parameter,
                            .role = LSPSymbolRole::Declaration,
                            .name = parameter.name,
                            .range = parameter.name_range,
                            .declaration_range = parameter.name_range,
                            .type = parameter.resolved_type,
                        });
                    }
                    for (const auto& reference :
                         parameterReferenceOccurrence(function_decl)) {
                        emit(reference);
                    }
                    if (function_decl.body != nullptr) {
                        visitBlock(*function_decl.body, type_parameters,
                                   &function_decl);
                    }
                },
            },
            decl);
    }

    auto visitBlock(const ast::Block& block,
                    const std::vector<TypeParameterInfo>& type_parameters,
                    const ast::FunctionDecl* enclosing_function) -> void {
        for (const auto& statement : block.statements) {
            visitStmt(*statement, type_parameters, enclosing_function);
        }
    }

    auto visitStmt(const ast::Stmt& stmt,
                   const std::vector<TypeParameterInfo>& type_parameters,
                   const ast::FunctionDecl* enclosing_function) -> void {
        noteStmt(stmt);
        std::visit(
            Overloaded{
                [&](const ast::VarDeclStmt& var_decl) {
                    visitType(*var_decl.type, type_parameters);
                    emit(LSPSymbolOccurrence{
                        .kind = LSPSymbolKind::Local,
                        .role = LSPSymbolRole::Declaration,
                        .name = var_decl.name,
                        .range = var_decl.name_range,
                        .declaration_range = var_decl.name_range,
                        .type = var_decl.type != nullptr
                                    ? var_decl.type->resolved_type
                                    : nullptr,
                    });
                    if (var_decl.initializer != nullptr) {
                        visitExpr(*var_decl.initializer, type_parameters,
                                  enclosing_function);
                    }
                },
                [&](const ast::ExprStmt& expr_stmt) {
                    visitExpr(*expr_stmt.expr, type_parameters,
                              enclosing_function);
                },
                [&](const ast::AssignStmt& assign) {
                    visitExpr(*assign.target, type_parameters,
                              enclosing_function);
                    visitExpr(*assign.value, type_parameters,
                              enclosing_function);
                },
                [&](const ast::UpdateStmt& update) {
                    visitExpr(*update.target, type_parameters,
                              enclosing_function);
                },
                [&](const ast::ReturnStmt& ret) {
                    if (ret.value != nullptr) {
                        visitExpr(*ret.value, type_parameters,
                                  enclosing_function);
                    }
                },
                [&](const ast::DropStmt& drop_stmt) {
                    visitExpr(*drop_stmt.value, type_parameters,
                              enclosing_function);
                },
                [&](const ast::IfStmt& if_stmt) {
                    visitExpr(*if_stmt.condition, type_parameters,
                              enclosing_function);
                    visitBlock(*if_stmt.then_block, type_parameters,
                               enclosing_function);
                    if (if_stmt.else_block != nullptr) {
                        visitBlock(*if_stmt.else_block, type_parameters,
                                   enclosing_function);
                    }
                },
                [&](const ast::WhileStmt& while_stmt) {
                    visitExpr(*while_stmt.condition, type_parameters,
                              enclosing_function);
                    visitBlock(*while_stmt.body, type_parameters,
                               enclosing_function);
                },
                [&](const ast::ForStmt& for_stmt) {
                    if (for_stmt.initializer != nullptr) {
                        visitStmt(*for_stmt.initializer, type_parameters,
                                  enclosing_function);
                    }
                    if (for_stmt.condition != nullptr) {
                        visitExpr(*for_stmt.condition, type_parameters,
                                  enclosing_function);
                    }
                    if (for_stmt.step != nullptr) {
                        visitStmt(*for_stmt.step, type_parameters,
                                  enclosing_function);
                    }
                    visitBlock(*for_stmt.body, type_parameters,
                               enclosing_function);
                },
                [&](const ast::BreakStmt&) {},
                [&](const ast::ContinueStmt&) {},
                [&](const ast::UncheckedStmt& unchecked_stmt) {
                    visitBlock(*unchecked_stmt.body, type_parameters,
                               enclosing_function);
                },
                [&](const ast::SwitchStmt& switch_stmt) {
                    visitExpr(*switch_stmt.scrutinee, type_parameters,
                              enclosing_function);
                    for (const auto& switch_case : switch_stmt.cases) {
                        if (const auto variant =
                                variantOccurrence(switch_case, switch_stmt);
                            variant.has_value()) {
                            emit(*variant);
                        }
                        if (switch_case.binding_name.has_value() &&
                            switch_case.binding_name_range.has_value()) {
                            emit(LSPSymbolOccurrence{
                                .kind = LSPSymbolKind::SwitchBinding,
                                .role = LSPSymbolRole::Declaration,
                                .name = *switch_case.binding_name,
                                .range = *switch_case.binding_name_range,
                                .declaration_range =
                                    switch_case.binding_name_range,
                                .type = switch_case.binding_type,
                            });
                        }
                        visitBlock(*switch_case.body, type_parameters,
                                   enclosing_function);
                    }
                },
                [&](const ast::Block& block) {
                    visitBlock(block, type_parameters, enclosing_function);
                },
            },
            stmt.node);
    }

    auto visitExpr(const ast::Expr& expr,
                   const std::vector<TypeParameterInfo>& type_parameters,
                   const ast::FunctionDecl* enclosing_function) -> void {
        static_cast<void>(enclosing_function);
        noteExpr(expr);
        std::visit(
            Overloaded{
                [&](const ast::IntegerLiteralExpr&) {},
                [&](const ast::FloatLiteralExpr&) {},
                [&](const ast::CharLiteralExpr&) {},
                [&](const ast::BoolLiteralExpr&) {},
                [&](const ast::StringLiteralExpr&) {},
                [&](const ast::NameExpr& name) {
                    if (const auto occurrence = nameOccurrence(name);
                        occurrence.has_value()) {
                        emit(*occurrence);
                    }
                },
                [&](const ast::UnaryExpr& unary) {
                    visitExpr(*unary.operand, type_parameters,
                              enclosing_function);
                },
                [&](const ast::BinaryExpr& binary) {
                    visitExpr(*binary.lhs, type_parameters, enclosing_function);
                    visitExpr(*binary.rhs, type_parameters, enclosing_function);
                },
                [&](const ast::CallExpr& call) {
                    if (const auto occurrence = callOccurrence(call);
                        occurrence.has_value()) {
                        emit(*occurrence);
                    }
                    for (const auto& argument : call.arguments) {
                        visitExpr(*argument, type_parameters,
                                  enclosing_function);
                    }
                },
                [&](const ast::MemberExpr& member) {
                    visitExpr(*member.base, type_parameters,
                              enclosing_function);
                    if (const auto occurrence = memberOccurrence(member);
                        occurrence.has_value()) {
                        emit(*occurrence);
                    }
                },
                [&](const ast::IndexExpr& index) {
                    visitExpr(*index.base, type_parameters, enclosing_function);
                    visitExpr(*index.index, type_parameters,
                              enclosing_function);
                },
                [&](const ast::InitListExpr& init_list) {
                    for (const auto& element : init_list.elements) {
                        visitExpr(*element, type_parameters,
                                  enclosing_function);
                    }
                },
                [&](const ast::ArrayLiteralExpr& array_literal) {
                    for (const auto& element : array_literal.elements) {
                        visitExpr(*element, type_parameters,
                                  enclosing_function);
                    }
                },
                [&](const ast::CastExpr& cast_expr) {
                    visitExpr(*cast_expr.operand, type_parameters,
                              enclosing_function);
                    visitType(*cast_expr.target_type, type_parameters);
                },
                [&](const ast::SizeofExpr& sizeof_expr) {
                    visitType(*sizeof_expr.type, type_parameters);
                },
            },
            expr.node);
    }

    auto visitType(const ast::TypeSyntax& type,
                   const std::vector<TypeParameterInfo>& type_parameters)
        -> void {
        noteType(type);
        if (type.element_type != nullptr) {
            visitType(*type.element_type, type_parameters);
        }
        for (const auto& type_argument : type.type_arguments) {
            visitType(*type_argument, type_parameters);
        }
        if (const auto occurrence = typeOccurrence(type, type_parameters);
            occurrence.has_value()) {
            emit(*occurrence);
        }
    }

    const ast::Module& module;
    const SemanticAnalysis& analysis;
    std::vector<LSPSymbolOccurrence>* symbols = nullptr;
    std::optional<std::size_t> query_offset;
    LSPQueryResult* query_result = nullptr;
    SourceRange best_symbol_range{};
};

} // namespace

LSPSupport::LSPSupport(const ast::Package& package,
                       const SemanticAnalysis& analysis)
    : package(package), analysis(analysis) {}

auto LSPSupport::moduleFor(const SourceFile& source) const
    -> const ast::Module* {
    const auto it =
        std::ranges::find_if(package.modules, [&](const auto& module) {
            return module->source == &source;
        });
    return it == package.modules.end() ? nullptr : it->get();
}

auto LSPSupport::offsetForLocation(const SourceFile& source,
                                   SourceLocation location) const
    -> std::optional<std::size_t> {
    if (location.line == 0 || location.column == 0) {
        return std::nullopt;
    }

    std::size_t current_line = 1;
    std::size_t line_start = 0;
    const auto text = source.text();
    while (current_line < location.line) {
        const auto newline = text.find('\n', line_start);
        if (newline == std::string_view::npos) {
            return std::nullopt;
        }
        line_start = newline + 1;
        ++current_line;
    }

    auto line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos) {
        line_end = text.size();
    }
    const auto line_length = line_end - line_start;
    const auto zero_based_column = location.column - 1;
    if (zero_based_column > line_length) {
        return std::nullopt;
    }
    return line_start + zero_based_column;
}

auto LSPSupport::query(const SourceFile& source, std::size_t offset) const
    -> std::optional<LSPQueryResult> {
    const auto* module = moduleFor(source);
    if (module == nullptr || offset > source.text().size()) {
        return std::nullopt;
    }

    LSPQueryResult result;
    ModuleTraversal traversal(*module, analysis, nullptr, offset, &result);
    traversal.traverse();
    return result;
}

auto LSPSupport::query(const SourceFile& source, SourceLocation location) const
    -> std::optional<LSPQueryResult> {
    const auto offset = offsetForLocation(source, location);
    if (!offset.has_value()) {
        return std::nullopt;
    }
    return query(source, *offset);
}

auto LSPSupport::documentSymbols(const SourceFile& source) const
    -> std::vector<LSPSymbolOccurrence> {
    std::vector<LSPSymbolOccurrence> occurrences;
    const auto* module = moduleFor(source);
    if (module == nullptr) {
        return occurrences;
    }

    ModuleTraversal traversal(*module, analysis, &occurrences, std::nullopt,
                              nullptr);
    traversal.traverse();
    return occurrences;
}

namespace lsp_impl {

constexpr glz::opts GLAZE_READ_OPTS{.error_on_unknown_keys = false};
constexpr int DIAGNOSTIC_SEVERITY_ERROR = 1;
constexpr int TEXT_DOCUMENT_SYNC_FULL = 1;

// NOLINTBEGIN(readability-identifier-naming)

struct JsonRpcMessage {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<glz::raw_json_view> params;
    std::optional<glz::raw_json_view> id;
};

struct JsonRpcError {
    int code = -32603;
    std::string message;
    std::optional<glz::raw_json> data;
};

struct JsonRpcResponse {
    std::string jsonrpc = "2.0";
    std::optional<glz::raw_json> result;
    std::optional<JsonRpcError> error;
    glz::raw_json id = glz::raw_json{"null"};
};

struct JsonRpcNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    std::optional<glz::raw_json> params;
};

struct LSPPosition {
    std::size_t line = 0;
    std::size_t character = 0;
};

struct LSPRange {
    LSPPosition start;
    LSPPosition end;
};

struct LSPDiagnostic {
    LSPRange range;
    int severity = DIAGNOSTIC_SEVERITY_ERROR;
    std::string source = "safe-c";
    std::string message;
};

struct PublishDiagnosticsParams {
    std::string uri;
    std::vector<LSPDiagnostic> diagnostics;
    std::optional<int> version;
};

struct TextDocumentIdentifier {
    std::string uri;
};

struct VersionedTextDocumentIdentifier {
    std::string uri;
    int version = 0;
};

struct TextDocumentItem {
    std::string uri;
    std::string languageId;
    int version = 0;
    std::string text;
};

struct DidOpenTextDocumentParams {
    TextDocumentItem textDocument;
};

struct TextDocumentContentChangeEvent {
    std::optional<LSPRange> range;
    std::optional<std::size_t> rangeLength;
    std::string text;
};

struct DidChangeTextDocumentParams {
    VersionedTextDocumentIdentifier textDocument;
    std::vector<TextDocumentContentChangeEvent> contentChanges;
};

struct DidCloseTextDocumentParams {
    TextDocumentIdentifier textDocument;
};

struct TextDocumentPositionParams {
    TextDocumentIdentifier textDocument;
    LSPPosition position;
};

struct RenameParams {
    TextDocumentIdentifier textDocument;
    LSPPosition position;
    std::string newName;
};

struct SemanticTokensParams {
    TextDocumentIdentifier textDocument;
};

struct SemanticTokens {
    std::vector<std::uint32_t> data;
};

struct MarkupContent {
    std::string kind = "markdown";
    std::string value;
};

struct Hover {
    MarkupContent contents;
    std::optional<LSPRange> range;
};

struct Location {
    std::string uri;
    LSPRange range;
};

struct TextEdit {
    LSPRange range;
    std::string newText;
};

struct WorkspaceEdit {
    std::unordered_map<std::string, std::vector<TextEdit>> changes;
};

struct TextDocumentSyncOptions {
    bool openClose = true;
    int change = TEXT_DOCUMENT_SYNC_FULL;
};

struct SemanticTokensLegend {
    std::vector<std::string> tokenTypes;
    std::vector<std::string> tokenModifiers;
};

struct SemanticTokensOptions {
    SemanticTokensLegend legend;
    bool full = true;
};

struct ServerCapabilities {
    TextDocumentSyncOptions textDocumentSync;
    bool hoverProvider = true;
    bool definitionProvider = true;
    bool renameProvider = true;
    SemanticTokensOptions semanticTokensProvider;
};

struct ServerInfo {
    std::string name = "safe-c";
    std::string version = "dev";
};

struct InitializeResult {
    ServerCapabilities capabilities;
    ServerInfo serverInfo;
};

struct OpenDocument {
    std::string uri;
    std::filesystem::path path;
    int version = 0;
    std::string text;
};

struct DocumentSnapshot {
    std::filesystem::path entry_path;
    ast::Package package;
    TypeContext types;
    SemanticAnalysis analysis;
    DiagnosticList diagnostics;
    const SourceFile* source = nullptr;
};

// NOLINTEND(readability-identifier-naming)

using SourceOverrideMap = std::unordered_map<std::string, std::string>;

auto range_equals(SourceRange lhs, SourceRange rhs) -> bool {
    return lhs.begin == rhs.begin && lhs.end == rhs.end &&
           lhs.source == rhs.source;
}

auto trim_ascii(std::string_view text) -> std::string_view {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

template <typename T>
auto parse_json(std::string_view json) -> std::expected<T, std::string> {
    T value{};
    if (auto ec = glz::read<GLAZE_READ_OPTS>(value, json); ec) {
        return std::unexpected(glz::format_error(ec, json));
    }
    return value;
}

template <typename T>
auto serialize_json(const T& value) -> std::expected<std::string, std::string> {
    auto json = glz::write_json(value);
    if (!json.has_value()) {
        return std::unexpected(glz::format_error(json.error()));
    }
    return json.value();
}

template <typename T>
auto to_raw_json(const T& value) -> std::expected<glz::raw_json, std::string> {
    auto json = serialize_json(value);
    if (!json.has_value()) {
        return std::unexpected(json.error());
    }
    return glz::raw_json{std::move(json.value())};
}

auto copy_raw_json(glz::raw_json_view value) -> glz::raw_json {
    return glz::raw_json{std::string(value.str)};
}

auto json_null() -> glz::raw_json { return glz::raw_json{"null"}; }

auto make_error_response(const std::optional<glz::raw_json_view>& request_id,
                         int code, std::string message,
                         std::optional<std::string> data = std::nullopt)
    -> std::expected<std::string, std::string> {
    JsonRpcResponse response;
    response.id = request_id.has_value() ? copy_raw_json(*request_id)
                                         : glz::raw_json{"null"};
    response.error = JsonRpcError{
        .code = code,
        .message = std::move(message),
        .data = data.has_value() ? std::optional<glz::raw_json>(
                                       glz::raw_json{std::move(*data)})
                                 : std::nullopt,
    };
    return serialize_json(response);
}

auto make_success_response(const std::optional<glz::raw_json_view>& request_id,
                           glz::raw_json result)
    -> std::expected<std::string, std::string> {
    JsonRpcResponse response;
    response.id = request_id.has_value() ? copy_raw_json(*request_id)
                                         : glz::raw_json{"null"};
    response.result = std::move(result);
    return serialize_json(response);
}

template <typename T>
auto make_notification(const std::string& method, const T& params)
    -> std::expected<std::string, std::string> {
    auto raw_params = to_raw_json(params);
    if (!raw_params.has_value()) {
        return std::unexpected(raw_params.error());
    }
    JsonRpcNotification notification{
        .jsonrpc = "2.0",
        .method = std::move(method),
        .params = std::move(raw_params.value()),
    };
    return serialize_json(notification);
}

template <typename T>
auto parse_params(const JsonRpcMessage& message)
    -> std::expected<T, std::string> {
    if (!message.params.has_value()) {
        return parse_json<T>("{}");
    }
    return parse_json<T>(message.params->str);
}

auto normalized_path(const std::filesystem::path& path)
    -> std::filesystem::path {
    return std::filesystem::absolute(path).lexically_normal();
}

auto path_key(const std::filesystem::path& path) -> std::string {
    return normalized_path(path).string();
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
    path.replace_extension(".sc");
    return path;
}

auto load_source_with_overrides(const std::filesystem::path& path,
                                const SourceOverrideMap& overrides)
    -> std::expected<SourceFile, std::string> {
    const auto normalized = normalized_path(path);
    if (const auto it = overrides.find(normalized.string());
        it != overrides.end()) {
        return SourceFile::fromText(normalized, it->second);
    }
    return SourceFile::load(normalized);
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

auto find_source_for_path(const ast::Package& package,
                          const std::filesystem::path& path)
    -> const SourceFile* {
    const auto target = normalized_path(path);
    for (const auto& source : package.sources) {
        if (normalized_path(source->path()) == target) {
            return source.get();
        }
    }
    return nullptr;
}

auto load_module(ast::Package& package, const std::filesystem::path& root_dir,
                 const std::filesystem::path& module_path,
                 std::string module_name,
                 std::unordered_map<std::string, ast::Module*>& loaded_modules,
                 const SourceOverrideMap& overrides)
    -> std::expected<ast::Module*, DiagnosticList> {
    const auto canonical_path = normalized_path(module_path);
    const auto key = canonical_path.string();
    if (const auto it = loaded_modules.find(key); it != loaded_modules.end()) {
        return it->second;
    }

    auto source = load_source_with_overrides(canonical_path, overrides);
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
        if (normalized_path(imported_path) == canonical_path) {
            return std::unexpected(DiagnosticList{
                Diagnostic("module cannot import itself", import_decl.range)});
        }
        auto imported_module =
            load_module(package, root_dir, imported_path,
                        import_decl.module_name, loaded_modules, overrides);
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
                  const std::filesystem::path& entry_path,
                  const SourceOverrideMap& overrides)
    -> std::expected<void, DiagnosticList> {
    std::unordered_map<std::string, ast::Module*> loaded_modules;

    const auto canonical_entry = normalized_path(entry_path);
    const auto root_dir = canonical_entry.parent_path();
    const auto module_name =
        module_name_from_relative_path(canonical_entry.filename());

    auto entry_module = load_module(package, root_dir, canonical_entry,
                                    module_name, loaded_modules, overrides);
    if (!entry_module) {
        return std::unexpected(entry_module.error());
    }
    package.entry_module = *entry_module;
    return {};
}

auto analyze_document(const std::filesystem::path& entry_path,
                      const SourceOverrideMap& overrides) -> DocumentSnapshot {
    DocumentSnapshot snapshot;
    snapshot.entry_path = normalized_path(entry_path);

    auto loaded =
        load_package(snapshot.package, snapshot.entry_path, overrides);
    snapshot.source =
        find_source_for_path(snapshot.package, snapshot.entry_path);
    snapshot.diagnostics = collect_package_diagnostics(snapshot.package);
    if (!loaded) {
        snapshot.diagnostics.insert(snapshot.diagnostics.end(),
                                    loaded.error().begin(),
                                    loaded.error().end());
        return snapshot;
    }

    SemanticAnalyzer sema(snapshot.types);
    snapshot.analysis = sema.analyze(snapshot.package);
    snapshot.diagnostics.insert(snapshot.diagnostics.end(),
                                snapshot.analysis.diagnostics.begin(),
                                snapshot.analysis.diagnostics.end());
    return snapshot;
}

auto hex_value(char ch) -> int {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

auto percent_decode(std::string_view encoded) -> std::string {
    std::string decoded;
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const char ch = encoded[index];
        if (ch == '%' && index + 2 < encoded.size()) {
            const int hi = hex_value(encoded[index + 1]);
            const int lo = hex_value(encoded[index + 2]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi * 16) + lo));
                index += 2;
                continue;
            }
        }
        decoded.push_back(ch);
    }
    return decoded;
}

auto percent_encode(std::string_view raw) -> std::string {
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(raw.size());
    for (const auto ch : raw) {
        const auto uch = static_cast<unsigned char>(ch);
        const bool unreserved =
            (uch >= 'A' && uch <= 'Z') || (uch >= 'a' && uch <= 'z') ||
            (uch >= '0' && uch <= '9') || uch == '-' || uch == '_' ||
            uch == '.' || uch == '~' || uch == '/';
        if (unreserved) {
            encoded.push_back(static_cast<char>(uch));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(HEX[uch >> 4]);
        encoded.push_back(HEX[uch & 0x0F]);
    }
    return encoded;
}

auto path_from_uri(std::string_view uri)
    -> std::optional<std::filesystem::path> {
    static constexpr std::string_view PREFIX = "file://";
    if (!uri.starts_with(PREFIX)) {
        return std::nullopt;
    }
    auto path = percent_decode(uri.substr(PREFIX.size()));
    if (path.size() >= 3 && path[0] == '/' && (std::isalpha(path[1]) != 0) &&
        path[2] == ':') {
        path.erase(path.begin());
    }
    return normalized_path(path);
}

auto uri_from_path(const std::filesystem::path& path) -> std::string {
    const auto normalized = normalized_path(path).generic_string();
    return "file://" + percent_encode(normalized);
}

class TextPositionConverter {
  public:
    explicit TextPositionConverter(std::string_view text) : text(text) {
        line_starts.push_back(0);
        for (std::size_t index = 0; index < text.size(); ++index) {
            if (text[index] == '\n') {
                line_starts.push_back(index + 1);
            }
        }
    }

    [[nodiscard]] auto positionForOffset(std::size_t offset) const
        -> LSPPosition {
        offset = std::min(offset, text.size());
        const auto line_it = std::ranges::upper_bound(line_starts, offset);
        const auto line_index = static_cast<std::size_t>(
            std::distance(line_starts.begin(), line_it) - 1);
        const auto line_start = line_starts[line_index];

        std::size_t byte_offset = line_start;
        std::size_t utf16_column = 0;
        while (byte_offset < offset) {
            const auto [length, units] = decode(text, byte_offset);
            byte_offset += length;
            utf16_column += units;
        }
        return LSPPosition{.line = line_index, .character = utf16_column};
    }

    [[nodiscard]] auto offsetForPosition(LSPPosition position) const
        -> std::optional<std::size_t> {
        if (position.line >= line_starts.size()) {
            return std::nullopt;
        }
        const auto line_start = line_starts[position.line];
        const auto line_end = position.line + 1 < line_starts.size()
                                  ? line_starts[position.line + 1] - 1
                                  : text.size();

        std::size_t byte_offset = line_start;
        std::size_t utf16_column = 0;
        while (byte_offset < line_end) {
            if (utf16_column == position.character) {
                return byte_offset;
            }
            const auto [length, units] = decode(text, byte_offset);
            if (utf16_column + units > position.character) {
                return byte_offset;
            }
            byte_offset += length;
            utf16_column += units;
        }
        if (utf16_column == position.character) {
            return byte_offset;
        }
        return std::nullopt;
    }

  private:
    static auto decode(std::string_view bytes, std::size_t offset)
        -> std::pair<std::size_t, std::size_t> {
        const auto lead = static_cast<unsigned char>(bytes[offset]);
        if (lead < 0x80) {
            return {1, 1};
        }
        if ((lead >> 5) == 0x6 && offset + 1 < bytes.size() &&
            isContinuation(bytes[offset + 1])) {
            return {2, 1};
        }
        if ((lead >> 4) == 0xE && offset + 2 < bytes.size() &&
            isContinuation(bytes[offset + 1]) &&
            isContinuation(bytes[offset + 2])) {
            return {3, 1};
        }
        if ((lead >> 3) == 0x1E && offset + 3 < bytes.size() &&
            isContinuation(bytes[offset + 1]) &&
            isContinuation(bytes[offset + 2]) &&
            isContinuation(bytes[offset + 3])) {
            return {4, 2};
        }
        return {1, 1};
    }

    static auto isContinuation(char byte) -> bool {
        return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
    }

    std::string_view text;
    std::vector<std::size_t> line_starts;
};

auto lsp_range_from_source_range(const SourceRange& range,
                                 const SourceFile& source,
                                 bool ensure_non_empty = false) -> LSPRange {
    const TextPositionConverter converter(source.text());
    const auto begin = std::min(range.begin, source.text().size());
    auto end = std::min(range.end, source.text().size());
    if (ensure_non_empty && end <= begin && begin < source.text().size()) {
        end = begin + 1;
    }
    return LSPRange{.start = converter.positionForOffset(begin),
                    .end = converter.positionForOffset(end)};
}

auto diagnostic_range(const Diagnostic& diagnostic, const SourceFile& source)
    -> LSPRange {
    if (diagnostic.hasRange()) {
        return lsp_range_from_source_range(diagnostic.range(), source, true);
    }
    return LSPRange{};
}

auto format_type(const Type* type) -> std::string {
    if (type == nullptr) {
        return "<unknown>";
    }
    if (type->is_const) {
        auto inner = *type;
        inner.is_const = false;
        const auto described = format_type(&inner);
        if (inner.kind == TypeKind::Pointer || inner.kind == TypeKind::Borrow ||
            inner.kind == TypeKind::Slice || inner.kind == TypeKind::Array) {
            return "const (" + described + ')';
        }
        return "const " + described;
    }

    switch (type->kind) {
    case TypeKind::Void:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
    case TypeKind::Struct:
    case TypeKind::Enum:
        return type->name;
    case TypeKind::Interface:
        return '&' + type->name;
    case TypeKind::Borrow:
        return '&' + std::string(type->is_mut ? "mut " : "") +
               format_type(type->element_type);
    case TypeKind::Pointer:
        return format_type(type->element_type) + '*';
    case TypeKind::Slice:
        return "[]" + format_type(type->element_type);
    case TypeKind::Array:
        return format_type(type->element_type) + '[' +
               std::to_string(type->array_size) + ']';
    }
    return "<unknown>";
}

auto format_type_syntax(const ast::TypeSyntax* type, const Type* resolved)
    -> std::string {
    if (type != nullptr && type->range.source != nullptr) {
        return std::string(type->range.source->slice(type->range));
    }
    return format_type(resolved);
}

auto format_template_parameters(const std::vector<std::string>& parameters)
    -> std::string {
    if (parameters.empty()) {
        return {};
    }

    std::string text = "<";
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0) {
            text += ", ";
        }
        text += parameters[index];
    }
    text += '>';
    return text;
}

auto format_parameter(const ast::Parameter& parameter) -> std::string {
    return format_type_syntax(parameter.type.get(), parameter.resolved_type) +
           ' ' + parameter.name;
}

auto format_function_signature(const ast::FunctionDecl& function)
    -> std::string {
    std::string signature =
        format_type_syntax(function.return_type.get(),
                           function.resolved_return_type) +
        ' ' + function.name +
        format_template_parameters(function.type_parameters) + '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        if (index != 0) {
            signature += ", ";
        }
        signature += format_parameter(function.parameters[index]);
    }
    signature += ')';
    return signature;
}

auto markdown_code_block(std::string text) -> std::string {
    return "```safe-c\n" + std::move(text) + "\n```";
}

auto find_module_for_source(const ast::Package& package,
                            const SourceFile& source) -> const ast::Module* {
    const auto it =
        std::ranges::find_if(package.modules, [&](const auto& module) {
            return module->source == &source;
        });
    return it == package.modules.end() ? nullptr : it->get();
}

auto find_function_decl(const ast::Package& package, SourceRange range)
    -> const ast::FunctionDecl* {
    if (range.source == nullptr) {
        return nullptr;
    }
    const auto* module = find_module_for_source(package, *range.source);
    if (module == nullptr) {
        return nullptr;
    }
    for (const auto& decl : module->declarations) {
        if (const auto* function = std::get_if<ast::FunctionDecl>(&decl);
            function != nullptr && range_equals(function->name_range, range)) {
            return function;
        }
    }
    return nullptr;
}

auto find_struct_decl(const ast::Package& package, SourceRange range)
    -> const ast::StructDecl* {
    if (range.source == nullptr) {
        return nullptr;
    }
    const auto* module = find_module_for_source(package, *range.source);
    if (module == nullptr) {
        return nullptr;
    }
    for (const auto& decl : module->declarations) {
        if (const auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
            struct_decl != nullptr &&
            range_equals(struct_decl->name_range, range)) {
            return struct_decl;
        }
    }
    return nullptr;
}

auto find_enum_decl(const ast::Package& package, SourceRange range)
    -> const ast::EnumDecl* {
    if (range.source == nullptr) {
        return nullptr;
    }
    const auto* module = find_module_for_source(package, *range.source);
    if (module == nullptr) {
        return nullptr;
    }
    for (const auto& decl : module->declarations) {
        if (const auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
            enum_decl != nullptr &&
            range_equals(enum_decl->name_range, range)) {
            return enum_decl;
        }
    }
    return nullptr;
}

auto find_interface_decl(const ast::Package& package, SourceRange range)
    -> const ast::InterfaceDecl* {
    if (range.source == nullptr) {
        return nullptr;
    }
    const auto* module = find_module_for_source(package, *range.source);
    if (module == nullptr) {
        return nullptr;
    }
    for (const auto& decl : module->declarations) {
        if (const auto* interface_decl = std::get_if<ast::InterfaceDecl>(&decl);
            interface_decl != nullptr &&
            range_equals(interface_decl->name_range, range)) {
            return interface_decl;
        }
    }
    return nullptr;
}

auto find_field_decl(const ast::Package& package, SourceRange range)
    -> const ast::StructField* {
    if (range.source == nullptr) {
        return nullptr;
    }
    const auto* module = find_module_for_source(package, *range.source);
    if (module == nullptr) {
        return nullptr;
    }
    for (const auto& decl : module->declarations) {
        const auto* struct_decl = std::get_if<ast::StructDecl>(&decl);
        if (struct_decl == nullptr) {
            continue;
        }
        for (const auto& field : struct_decl->fields) {
            if (range_equals(field.name_range, range)) {
                return &field;
            }
        }
    }
    return nullptr;
}

auto find_variant_decl(const ast::Package& package, SourceRange range)
    -> const ast::EnumVariant* {
    if (range.source == nullptr) {
        return nullptr;
    }
    const auto* module = find_module_for_source(package, *range.source);
    if (module == nullptr) {
        return nullptr;
    }
    for (const auto& decl : module->declarations) {
        const auto* enum_decl = std::get_if<ast::EnumDecl>(&decl);
        if (enum_decl == nullptr) {
            continue;
        }
        for (const auto& variant : enum_decl->variants) {
            if (range_equals(variant.name_range, range)) {
                return &variant;
            }
        }
    }
    return nullptr;
}

auto find_local_symbol(const SemanticAnalysis& analysis, SourceRange range)
    -> const SemanticLocalSymbol* {
    for (const auto& [_, symbol] : analysis.local_symbols) {
        if (range_equals(symbol.name_range, range)) {
            return &symbol;
        }
    }
    return nullptr;
}

auto build_hover(const DocumentSnapshot& snapshot, const SourceFile& source,
                 const LSPQueryResult& query) -> std::optional<Hover> {
    std::string contents;
    std::optional<LSPRange> range;

    if (query.symbol.has_value()) {
        const auto& symbol = *query.symbol;
        range = lsp_range_from_source_range(symbol.range, source);

        switch (symbol.kind) {
        case LSPSymbolKind::BuiltinType:
            contents = markdown_code_block("type " + symbol.name);
            break;
        case LSPSymbolKind::Struct:
            if (symbol.declaration_range.has_value()) {
                if (const auto* decl = find_struct_decl(
                        snapshot.package, *symbol.declaration_range);
                    decl != nullptr) {
                    contents = markdown_code_block(
                        "struct " + decl->name +
                        format_template_parameters(decl->type_parameters));
                    break;
                }
            }
            contents = markdown_code_block("struct " + symbol.name);
            break;
        case LSPSymbolKind::Enum:
            if (symbol.declaration_range.has_value()) {
                if (const auto* decl = find_enum_decl(
                        snapshot.package, *symbol.declaration_range);
                    decl != nullptr) {
                    contents = markdown_code_block(
                        "enum " + decl->name +
                        format_template_parameters(decl->type_parameters));
                    break;
                }
            }
            contents = markdown_code_block("enum " + symbol.name);
            break;
        case LSPSymbolKind::Interface:
            if (symbol.declaration_range.has_value()) {
                if (const auto* decl = find_interface_decl(
                        snapshot.package, *symbol.declaration_range);
                    decl != nullptr) {
                    contents = markdown_code_block("interface " + decl->name);
                    break;
                }
            }
            contents = markdown_code_block("interface " + symbol.name);
            break;
        case LSPSymbolKind::Function:
            if (symbol.declaration_range.has_value()) {
                if (const auto* decl = find_function_decl(
                        snapshot.package, *symbol.declaration_range);
                    decl != nullptr) {
                    contents =
                        markdown_code_block(format_function_signature(*decl));
                    break;
                }
            }
            if (symbol.type != nullptr) {
                contents = markdown_code_block(symbol.name + " -> " +
                                               format_type(symbol.type));
            } else {
                contents = markdown_code_block(symbol.name);
            }
            break;
        case LSPSymbolKind::Field:
            if (symbol.declaration_range.has_value()) {
                if (const auto* field = find_field_decl(
                        snapshot.package, *symbol.declaration_range);
                    field != nullptr) {
                    contents = markdown_code_block(
                        field->name + ": " +
                        format_type_syntax(field->type.get(),
                                           field->resolved_type));
                    break;
                }
            }
            contents = markdown_code_block(symbol.name + ": " +
                                           format_type(symbol.type));
            break;
        case LSPSymbolKind::Variant:
            if (symbol.declaration_range.has_value()) {
                if (const auto* variant = find_variant_decl(
                        snapshot.package, *symbol.declaration_range);
                    variant != nullptr) {
                    std::string signature = variant->name;
                    if (variant->payload_type != nullptr) {
                        signature +=
                            '(' +
                            format_type_syntax(variant->payload_type.get(),
                                               variant->resolved_type) +
                            ')';
                    }
                    contents = markdown_code_block(signature);
                    break;
                }
            }
            contents = markdown_code_block(symbol.name);
            break;
        case LSPSymbolKind::TypeParameter:
            contents = markdown_code_block("type parameter " + symbol.name);
            break;
        case LSPSymbolKind::Parameter:
        case LSPSymbolKind::Local:
        case LSPSymbolKind::SwitchBinding:
            contents = markdown_code_block(symbol.name + ": " +
                                           format_type(symbol.type));
            break;
        case LSPSymbolKind::ImportModule:
            contents = markdown_code_block("module " + symbol.name);
            break;
        }
    } else if (query.expression != nullptr &&
               query.expression->resolved_type != nullptr) {
        contents =
            markdown_code_block(format_type(query.expression->resolved_type));
        range = lsp_range_from_source_range(query.expression->range, source);
    } else if (query.type_syntax != nullptr) {
        contents = markdown_code_block(format_type_syntax(
            query.type_syntax, query.type_syntax->resolved_type));
        range = lsp_range_from_source_range(query.type_syntax->range, source);
    }

    if (contents.empty()) {
        return std::nullopt;
    }
    return Hover{
        .contents =
            MarkupContent{.kind = "markdown", .value = std::move(contents)},
        .range = range,
    };
}

auto redirected_declaration_range(const ast::Package& package,
                                  const LSPSymbolOccurrence& occurrence)
    -> std::optional<SourceRange> {
    if (!occurrence.declaration_range.has_value()) {
        return std::nullopt;
    }
    if (occurrence.kind == LSPSymbolKind::Function &&
        occurrence.role == LSPSymbolRole::Declaration) {
        if (const auto* function =
                find_function_decl(package, occurrence.range);
            function != nullptr &&
            function->impl_target_kind != ast::ImplTargetKind::None &&
            function->interface_decl != nullptr) {
            return function->interface_decl->name_range;
        }
    }
    return occurrence.declaration_range;
}

auto is_symbol_renamable(LSPSymbolKind kind) -> bool {
    switch (kind) {
    case LSPSymbolKind::BuiltinType:
    case LSPSymbolKind::ImportModule:
        return false;
    case LSPSymbolKind::Struct:
    case LSPSymbolKind::Enum:
    case LSPSymbolKind::Interface:
    case LSPSymbolKind::Function:
    case LSPSymbolKind::Field:
    case LSPSymbolKind::Variant:
    case LSPSymbolKind::TypeParameter:
    case LSPSymbolKind::Parameter:
    case LSPSymbolKind::Local:
    case LSPSymbolKind::SwitchBinding:
        return true;
    }
    return false;
}

auto is_valid_identifier_name(std::string_view name) -> bool {
    static const std::unordered_set<std::string_view> keywords = {
        "as",        "bool",    "break",   "case",      "char",   "const",
        "continue",  "default", "depends", "drop",      "else",   "enum",
        "export",    "extern",  "false",   "float",     "for",    "if",
        "impl",      "import",  "int",     "interface", "move",   "mut",
        "on",        "return",  "sizeof",  "struct",    "switch", "true",
        "unchecked", "void",    "while"};
    if (name.empty()) {
        return false;
    }
    const auto first = static_cast<unsigned char>(name.front());
    if (std::isalpha(first) == 0 && first != '_') {
        return false;
    }
    for (const auto ch : name) {
        const auto uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) == 0 && ch != '_') {
            return false;
        }
    }
    return !keywords.contains(name);
}

auto build_definition_location(const ast::Package& package,
                               const LSPQueryResult& query)
    -> std::optional<Location> {
    if (!query.symbol.has_value()) {
        return std::nullopt;
    }

    const auto declaration_range =
        redirected_declaration_range(package, *query.symbol);
    if (!declaration_range.has_value() ||
        declaration_range->source == nullptr) {
        return std::nullopt;
    }

    const auto& declaration_source = *declaration_range->source;
    return Location{
        .uri = uri_from_path(declaration_source.path()),
        .range =
            lsp_range_from_source_range(*declaration_range, declaration_source),
    };
}

auto build_workspace_edit(const DocumentSnapshot& snapshot,
                          SourceRange target_declaration_range,
                          std::string_view new_name)
    -> std::optional<WorkspaceEdit> {
    if (target_declaration_range.source == nullptr) {
        return std::nullopt;
    }

    const LSPSupport lsp(snapshot.package, snapshot.analysis);
    WorkspaceEdit edit;
    std::unordered_set<std::string> seen_ranges;

    for (const auto& module : snapshot.package.modules) {
        if (module == nullptr || module->source == nullptr) {
            continue;
        }

        for (const auto& occurrence : lsp.documentSymbols(*module->source)) {
            if (!is_symbol_renamable(occurrence.kind) ||
                occurrence.range.source == nullptr) {
                continue;
            }

            const auto occurrence_target =
                redirected_declaration_range(snapshot.package, occurrence);
            if (!occurrence_target.has_value() ||
                !range_equals(*occurrence_target, target_declaration_range)) {
                continue;
            }

            const auto uri = uri_from_path(occurrence.range.source->path());
            const auto dedupe_key = uri + "|" +
                                    std::to_string(occurrence.range.begin) +
                                    ":" + std::to_string(occurrence.range.end);
            if (!seen_ranges.insert(dedupe_key).second) {
                continue;
            }

            edit.changes[uri].push_back(TextEdit{
                .range = lsp_range_from_source_range(occurrence.range,
                                                     *occurrence.range.source),
                .newText = std::string(new_name),
            });
        }
    }

    if (edit.changes.empty()) {
        return std::nullopt;
    }

    for (auto& [_, edits] : edit.changes) {
        std::ranges::sort(edits, [](const TextEdit& lhs, const TextEdit& rhs) {
            if (lhs.range.start.line != rhs.range.start.line) {
                return lhs.range.start.line > rhs.range.start.line;
            }
            if (lhs.range.start.character != rhs.range.start.character) {
                return lhs.range.start.character > rhs.range.start.character;
            }
            if (lhs.range.end.line != rhs.range.end.line) {
                return lhs.range.end.line > rhs.range.end.line;
            }
            return lhs.range.end.character > rhs.range.end.character;
        });
    }

    return edit;
}

enum class SemanticTokenTypeIndex : std::uint8_t {
    Namespace = 0,
    Type = 1,
    Class = 2,
    Enum = 3,
    Interface = 4,
    TypeParameter = 5,
    Parameter = 6,
    Variable = 7,
    Property = 8,
    Function = 9,
    EnumMember = 10,
    Keyword = 11,
    Number = 12,
    String = 13,
    Operator = 14,
};

struct SemanticTokenEntry {
    SourceRange range;
    std::uint32_t type = 0;
    std::uint32_t modifiers = 0;
};

auto semantic_token_type(const LSPSymbolOccurrence& occurrence)
    -> std::uint32_t {
    switch (occurrence.kind) {
    case LSPSymbolKind::ImportModule:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Namespace);
    case LSPSymbolKind::BuiltinType:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Type);
    case LSPSymbolKind::Struct:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Class);
    case LSPSymbolKind::Enum:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Enum);
    case LSPSymbolKind::Interface:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Interface);
    case LSPSymbolKind::TypeParameter:
        return static_cast<std::uint32_t>(
            SemanticTokenTypeIndex::TypeParameter);
    case LSPSymbolKind::Parameter:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Parameter);
    case LSPSymbolKind::Local:
    case LSPSymbolKind::SwitchBinding:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Variable);
    case LSPSymbolKind::Field:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Property);
    case LSPSymbolKind::Function:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Function);
    case LSPSymbolKind::Variant:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::EnumMember);
    }
    return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Variable);
}

auto semantic_token_modifiers(const LSPSymbolOccurrence& occurrence)
    -> std::uint32_t {
    std::uint32_t modifiers = 0;
    if (occurrence.role == LSPSymbolRole::Declaration) {
        modifiers |= 1U << 0U;
    }
    if (occurrence.kind == LSPSymbolKind::BuiltinType) {
        modifiers |= 1U << 1U;
    }
    return modifiers;
}

auto semantic_token_entries_for_symbols(
    const std::vector<LSPSymbolOccurrence>& occurrences)
    -> std::vector<SemanticTokenEntry> {
    std::vector<SemanticTokenEntry> entries;
    entries.reserve(occurrences.size());
    for (const auto& occurrence : occurrences) {
        entries.push_back(SemanticTokenEntry{
            .range = occurrence.range,
            .type = semantic_token_type(occurrence),
            .modifiers = semantic_token_modifiers(occurrence),
        });
    }
    return entries;
}

auto is_builtin_type_keyword(TokenKind kind) -> bool {
    switch (kind) {
    case TokenKind::KwBool:
    case TokenKind::KwChar:
    case TokenKind::KwFloat:
    case TokenKind::KwInt:
    case TokenKind::KwVoid:
        return true;
    default:
        return false;
    }
}

auto is_semantic_keyword(TokenKind kind) -> bool {
    switch (kind) {
    case TokenKind::True:
    case TokenKind::False:
    case TokenKind::KwAs:
    case TokenKind::KwCase:
    case TokenKind::KwBreak:
    case TokenKind::KwConst:
    case TokenKind::KwContinue:
    case TokenKind::KwDefault:
    case TokenKind::KwDepends:
    case TokenKind::KwDrop:
    case TokenKind::KwElse:
    case TokenKind::KwEnum:
    case TokenKind::KwExport:
    case TokenKind::KwExtern:
    case TokenKind::KwFor:
    case TokenKind::KwImpl:
    case TokenKind::KwIf:
    case TokenKind::KwImport:
    case TokenKind::KwInterface:
    case TokenKind::KwMove:
    case TokenKind::KwMut:
    case TokenKind::KwOn:
    case TokenKind::KwReturn:
    case TokenKind::KwSizeof:
    case TokenKind::KwStruct:
    case TokenKind::KwSwitch:
    case TokenKind::KwUnchecked:
    case TokenKind::KwWhile:
        return !is_builtin_type_keyword(kind);
    default:
        return false;
    }
}

auto is_semantic_operator(TokenKind kind) -> bool {
    switch (kind) {
    case TokenKind::Ampersand:
    case TokenKind::Star:
    case TokenKind::Dot:
    case TokenKind::Equal:
    case TokenKind::EqualEqual:
    case TokenKind::Bang:
    case TokenKind::BangEqual:
    case TokenKind::Plus:
    case TokenKind::PlusPlus:
    case TokenKind::Minus:
    case TokenKind::MinusMinus:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::AndAnd:
    case TokenKind::OrOr:
        return true;
    default:
        return false;
    }
}

auto syntax_token_type(const Token& token) -> std::optional<std::uint32_t> {
    switch (token.kind) {
    case TokenKind::Integer:
    case TokenKind::Float:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Number);
    case TokenKind::Character:
    case TokenKind::String:
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::String);
    default:
        break;
    }

    if (is_semantic_keyword(token.kind)) {
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Keyword);
    }
    if (is_semantic_operator(token.kind)) {
        return static_cast<std::uint32_t>(SemanticTokenTypeIndex::Operator);
    }
    return std::nullopt;
}

auto ranges_overlap(SourceRange lhs, SourceRange rhs) -> bool {
    if (lhs.source != rhs.source) {
        return false;
    }
    return lhs.begin < rhs.end && rhs.begin < lhs.end;
}

auto semantic_token_entries_for_syntax(const SourceFile& source)
    -> std::vector<SemanticTokenEntry> {
    Lexer lexer(source);
    const auto lexed = lexer.lexAll();
    if (!lexed.has_value()) {
        return {};
    }

    std::vector<SemanticTokenEntry> entries;
    entries.reserve(lexed->size());
    for (const auto& token : *lexed) {
        const auto token_type = syntax_token_type(token);
        if (!token_type.has_value() || token.range.source != &source ||
            token.range.end <= token.range.begin) {
            continue;
        }
        entries.push_back(SemanticTokenEntry{
            .range = token.range,
            .type = *token_type,
            .modifiers = 0,
        });
    }
    return entries;
}

auto build_semantic_tokens(const DocumentSnapshot& snapshot,
                           const SourceFile& source) -> SemanticTokens {
    SemanticTokens tokens;
    const LSPSupport lsp(snapshot.package, snapshot.analysis);
    auto occurrences = lsp.documentSymbols(source);
    auto entries = semantic_token_entries_for_symbols(occurrences);
    for (const auto& syntax_entry : semantic_token_entries_for_syntax(source)) {
        const auto overlaps_symbol = std::ranges::any_of(
            entries, [&](const SemanticTokenEntry& symbol_entry) {
                return ranges_overlap(symbol_entry.range, syntax_entry.range);
            });
        if (!overlaps_symbol) {
            entries.push_back(syntax_entry);
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const SemanticTokenEntry& lhs, const SemanticTokenEntry& rhs) {
                  if (lhs.range.begin != rhs.range.begin) {
                      return lhs.range.begin < rhs.range.begin;
                  }
                  if (lhs.range.end != rhs.range.end) {
                      return lhs.range.end < rhs.range.end;
                  }
                  if (lhs.type != rhs.type) {
                      return lhs.type < rhs.type;
                  }
                  return lhs.modifiers < rhs.modifiers;
              });

    const TextPositionConverter converter(source.text());
    std::optional<LSPPosition> previous;
    for (const auto& entry : entries) {
        if (entry.range.source != &source ||
            entry.range.end <= entry.range.begin) {
            continue;
        }
        const auto start = converter.positionForOffset(entry.range.begin);
        const auto end = converter.positionForOffset(entry.range.end);
        if (start.line != end.line || end.character <= start.character) {
            continue;
        }

        const auto delta_line =
            previous.has_value() ? start.line - previous->line : start.line;
        const auto delta_character = previous.has_value() && delta_line == 0
                                         ? start.character - previous->character
                                         : start.character;

        tokens.data.push_back(static_cast<std::uint32_t>(delta_line));
        tokens.data.push_back(static_cast<std::uint32_t>(delta_character));
        tokens.data.push_back(
            static_cast<std::uint32_t>(end.character - start.character));
        tokens.data.push_back(entry.type);
        tokens.data.push_back(entry.modifiers);
        previous = start;
    }
    return tokens;
}

auto semantic_token_legend() -> SemanticTokensLegend {
    return SemanticTokensLegend{
        .tokenTypes = {"namespace", "type", "class", "enum", "interface",
                       "typeParameter", "parameter", "variable", "property",
                       "function", "enumMember", "keyword", "number", "string",
                       "operator"},
        .tokenModifiers = {"declaration", "defaultLibrary"},
    };
}

auto initialize_result() -> InitializeResult {
    return InitializeResult{
        .capabilities =
            ServerCapabilities{
                .textDocumentSync = TextDocumentSyncOptions{},
                .hoverProvider = true,
                .definitionProvider = true,
                .renameProvider = true,
                .semanticTokensProvider =
                    SemanticTokensOptions{.legend = semantic_token_legend(),
                                          .full = true},
            },
        .serverInfo = ServerInfo{},
    };
}

auto parse_content_length(std::string_view header)
    -> std::optional<std::size_t> {
    static constexpr std::string_view PREFIX = "Content-Length:";
    if (!header.starts_with(PREFIX)) {
        return std::nullopt;
    }
    header.remove_prefix(PREFIX.size());
    header = trim_ascii(header);
    std::size_t value = 0;
    const auto* begin = header.data();
    const auto* end = header.data() + header.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

auto read_next_message(std::istream& input) -> std::optional<std::string> {
    std::string line;
    std::optional<std::size_t> content_length;
    bool saw_header = false;

    while (std::getline(input, line)) {
        saw_header = true;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }
        if (const auto length = parse_content_length(line);
            length.has_value()) {
            content_length = length;
        }
    }

    if (!saw_header || !content_length.has_value()) {
        return std::nullopt;
    }

    std::string body(*content_length, '\0');
    input.read(body.data(), static_cast<std::streamsize>(*content_length));
    if (input.gcount() != static_cast<std::streamsize>(*content_length)) {
        return std::nullopt;
    }
    return body;
}

auto write_message(std::ostream& output, std::string_view body) -> void {
    output << "Content-Length: " << body.size() << "\r\n\r\n";
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    output.flush();
}

class LanguageServerState {
  public:
    auto handle(std::string_view body) -> std::vector<std::string> {
        std::vector<std::string> outgoing;

        auto message = parse_json<JsonRpcMessage>(body);
        if (!message.has_value()) {
            if (auto response = make_error_response(
                    std::nullopt, -32700, "Parse error", message.error());
                response.has_value()) {
                outgoing.push_back(std::move(response.value()));
            }
            return outgoing;
        }

        const auto& request = *message;
        if (request.jsonrpc != "2.0" || request.method.empty()) {
            if (request.id.has_value()) {
                if (auto response = make_error_response(request.id, -32600,
                                                        "Invalid Request");
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
            }
            return outgoing;
        }

        if (request.method == "initialize") {
            auto result = to_raw_json(initialize_result());
            if (result.has_value()) {
                if (auto response =
                        make_success_response(request.id, std::move(*result));
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
            }
            return outgoing;
        }

        if (request.method == "initialized") {
            return outgoing;
        }

        if (request.method == "shutdown") {
            shutdown_requested = true;
            if (request.id.has_value()) {
                if (auto response =
                        make_success_response(request.id, json_null());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
            }
            return outgoing;
        }

        if (request.method == "exit") {
            exit_requested = true;
            return outgoing;
        }

        if (request.method == "textDocument/didOpen") {
            if (const auto params =
                    parse_params<DidOpenTextDocumentParams>(request);
                params.has_value()) {
                handleOpen(params->textDocument);
                append(outgoing, rebuildSnapshots());
            }
            return outgoing;
        }

        if (request.method == "textDocument/didChange") {
            if (const auto params =
                    parse_params<DidChangeTextDocumentParams>(request);
                params.has_value()) {
                handleChange(*params);
                append(outgoing, rebuildSnapshots());
            }
            return outgoing;
        }

        if (request.method == "textDocument/didClose") {
            if (const auto params =
                    parse_params<DidCloseTextDocumentParams>(request);
                params.has_value()) {
                open_documents.erase(params->textDocument.uri);
                snapshots.erase(params->textDocument.uri);
                append(outgoing, rebuildSnapshots());
            }
            return outgoing;
        }

        if (request.method == "textDocument/hover") {
            if (!request.id.has_value()) {
                return outgoing;
            }
            const auto params =
                parse_params<TextDocumentPositionParams>(request);
            if (!params.has_value()) {
                if (auto response = make_error_response(
                        request.id, -32602, "Invalid params", params.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            const auto hover = hoverFor(*params);
            auto raw_result =
                hover.has_value()
                    ? to_raw_json(*hover)
                    : std::expected<glz::raw_json, std::string>(json_null());
            if (!raw_result.has_value()) {
                if (auto response = make_error_response(request.id, -32603,
                                                        "Internal error",
                                                        raw_result.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            if (auto response =
                    make_success_response(request.id, std::move(*raw_result));
                response.has_value()) {
                outgoing.push_back(std::move(response.value()));
            }
            return outgoing;
        }

        if (request.method == "textDocument/definition") {
            if (!request.id.has_value()) {
                return outgoing;
            }
            const auto params =
                parse_params<TextDocumentPositionParams>(request);
            if (!params.has_value()) {
                if (auto response = make_error_response(
                        request.id, -32602, "Invalid params", params.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            const auto definition = definitionFor(*params);
            auto raw_result =
                definition.has_value()
                    ? to_raw_json(*definition)
                    : std::expected<glz::raw_json, std::string>(json_null());
            if (!raw_result.has_value()) {
                if (auto response = make_error_response(request.id, -32603,
                                                        "Internal error",
                                                        raw_result.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            if (auto response =
                    make_success_response(request.id, std::move(*raw_result));
                response.has_value()) {
                outgoing.push_back(std::move(response.value()));
            }
            return outgoing;
        }

        if (request.method == "textDocument/rename") {
            if (!request.id.has_value()) {
                return outgoing;
            }
            const auto params = parse_params<RenameParams>(request);
            if (!params.has_value()) {
                if (auto response = make_error_response(
                        request.id, -32602, "Invalid params", params.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }
            if (!is_valid_identifier_name(params->newName)) {
                if (auto response = make_error_response(
                        request.id, -32602, "Invalid params",
                        "rename target must be a non-keyword identifier");
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            const auto edit = renameFor(*params);
            auto raw_result =
                edit.has_value()
                    ? to_raw_json(*edit)
                    : std::expected<glz::raw_json, std::string>(json_null());
            if (!raw_result.has_value()) {
                if (auto response = make_error_response(request.id, -32603,
                                                        "Internal error",
                                                        raw_result.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            if (auto response =
                    make_success_response(request.id, std::move(*raw_result));
                response.has_value()) {
                outgoing.push_back(std::move(response.value()));
            }
            return outgoing;
        }

        if (request.method == "textDocument/semanticTokens/full") {
            if (!request.id.has_value()) {
                return outgoing;
            }
            const auto params = parse_params<SemanticTokensParams>(request);
            if (!params.has_value()) {
                if (auto response = make_error_response(
                        request.id, -32602, "Invalid params", params.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            const auto tokens = semanticTokensFor(*params);
            auto raw_result = to_raw_json(tokens);
            if (!raw_result.has_value()) {
                if (auto response = make_error_response(request.id, -32603,
                                                        "Internal error",
                                                        raw_result.error());
                    response.has_value()) {
                    outgoing.push_back(std::move(response.value()));
                }
                return outgoing;
            }

            if (auto response =
                    make_success_response(request.id, std::move(*raw_result));
                response.has_value()) {
                outgoing.push_back(std::move(response.value()));
            }
            return outgoing;
        }

        if (!request.id.has_value()) {
            return outgoing;
        }

        if (auto response =
                make_error_response(request.id, -32601, "Method not found");
            response.has_value()) {
            outgoing.push_back(std::move(response.value()));
        }
        return outgoing;
    }

    [[nodiscard]] auto exitRequested() const -> bool { return exit_requested; }
    [[nodiscard]] auto exitCode() const -> int {
        return shutdown_requested ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  private:
    [[nodiscard]] auto queryForHover(const DocumentSnapshot& snapshot,
                                     LSPPosition position) const
        -> std::optional<LSPQueryResult> {
        const TextPositionConverter converter(snapshot.source->text());
        const auto offset = converter.offsetForPosition(position);
        if (!offset.has_value()) {
            return std::nullopt;
        }

        const LSPSupport lsp(snapshot.package, snapshot.analysis);
        const auto query_at = [&](std::size_t candidate_offset) {
            return lsp.query(*snapshot.source, candidate_offset);
        };

        if (const auto query = query_at(*offset);
            query.has_value() &&
            (query->symbol.has_value() || query->expression != nullptr ||
             query->type_syntax != nullptr)) {
            return query;
        }
        if (*offset > 0) {
            if (const auto query = query_at(*offset - 1);
                query.has_value() &&
                (query->symbol.has_value() || query->expression != nullptr ||
                 query->type_syntax != nullptr)) {
                return query;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto queryForSymbol(const DocumentSnapshot& snapshot,
                                      LSPPosition position) const
        -> std::optional<LSPQueryResult> {
        const TextPositionConverter converter(snapshot.source->text());
        const auto offset = converter.offsetForPosition(position);
        if (!offset.has_value()) {
            return std::nullopt;
        }

        const LSPSupport lsp(snapshot.package, snapshot.analysis);
        const auto query_at = [&](std::size_t candidate_offset) {
            return lsp.query(*snapshot.source, candidate_offset);
        };

        if (const auto query = query_at(*offset);
            query.has_value() && query->symbol.has_value()) {
            return query;
        }
        if (*offset > 0) {
            if (const auto query = query_at(*offset - 1);
                query.has_value() && query->symbol.has_value()) {
                return query;
            }
        }
        return std::nullopt;
    }

    static auto append(std::vector<std::string>& target,
                       std::vector<std::string> source) -> void {
        target.insert(target.end(), std::make_move_iterator(source.begin()),
                      std::make_move_iterator(source.end()));
    }

    auto handleOpen(const TextDocumentItem& document) -> void {
        const auto path = path_from_uri(document.uri);
        if (!path.has_value()) {
            return;
        }
        open_documents[document.uri] = OpenDocument{.uri = document.uri,
                                                    .path = *path,
                                                    .version = document.version,
                                                    .text = document.text};
    }

    auto handleChange(const DidChangeTextDocumentParams& params) -> void {
        const auto it = open_documents.find(params.textDocument.uri);
        if (it == open_documents.end()) {
            return;
        }
        if (!params.contentChanges.empty()) {
            it->second.text = params.contentChanges.back().text;
        }
        it->second.version = params.textDocument.version;
    }

    [[nodiscard]] auto buildOverrides() const -> SourceOverrideMap {
        SourceOverrideMap overrides;
        overrides.reserve(open_documents.size());
        for (const auto& [_, document] : open_documents) {
            overrides.emplace(path_key(document.path), document.text);
        }
        return overrides;
    }

    [[nodiscard]] auto orderedOpenUris() const -> std::vector<std::string> {
        std::vector<std::string> uris;
        uris.reserve(open_documents.size());
        for (const auto& [uri, _] : open_documents) {
            uris.push_back(uri);
        }
        std::ranges::sort(uris);
        return uris;
    }

    auto rebuildSnapshots() -> std::vector<std::string> {
        snapshots.clear();
        const auto overrides = buildOverrides();
        for (const auto& uri : orderedOpenUris()) {
            const auto it = open_documents.find(uri);
            if (it == open_documents.end()) {
                continue;
            }
            snapshots.emplace(uri,
                              analyze_document(it->second.path, overrides));
        }
        return publishDiagnostics();
    }

    auto publishDiagnostics() -> std::vector<std::string> {
        std::unordered_map<std::string, std::vector<LSPDiagnostic>>
            merged_diagnostics;
        std::unordered_set<std::string> seen_keys;

        for (const auto& [_, snapshot] : snapshots) {
            for (const auto& diagnostic : snapshot.diagnostics) {
                const auto* source = diagnostic.source() != nullptr
                                         ? diagnostic.source()
                                         : snapshot.source;
                if (source == nullptr) {
                    continue;
                }
                const auto uri = uri_from_path(source->path());
                auto converted = LSPDiagnostic{
                    .range = diagnostic_range(diagnostic, *source),
                    .severity = DIAGNOSTIC_SEVERITY_ERROR,
                    .source = "safe-c",
                    .message = diagnostic.message(),
                };

                const auto& start = converted.range.start;
                const auto& end = converted.range.end;
                const auto key = uri + "|" + std::to_string(start.line) + ":" +
                                 std::to_string(start.character) + "-" +
                                 std::to_string(end.line) + ":" +
                                 std::to_string(end.character) + "|" +
                                 converted.message;
                if (seen_keys.insert(key).second) {
                    merged_diagnostics[uri].push_back(std::move(converted));
                }
            }
        }

        for (auto& [_, diagnostics] : merged_diagnostics) {
            std::ranges::sort(diagnostics, [](const LSPDiagnostic& lhs,
                                              const LSPDiagnostic& rhs) {
                if (lhs.range.start.line != rhs.range.start.line) {
                    return lhs.range.start.line < rhs.range.start.line;
                }
                if (lhs.range.start.character != rhs.range.start.character) {
                    return lhs.range.start.character <
                           rhs.range.start.character;
                }
                if (lhs.range.end.line != rhs.range.end.line) {
                    return lhs.range.end.line < rhs.range.end.line;
                }
                if (lhs.range.end.character != rhs.range.end.character) {
                    return lhs.range.end.character < rhs.range.end.character;
                }
                return lhs.message < rhs.message;
            });
        }

        std::vector<std::string> uris;
        uris.reserve(merged_diagnostics.size() +
                     last_published_diagnostics.size() + open_documents.size());
        for (const auto& [uri, _] : merged_diagnostics) {
            uris.push_back(uri);
        }
        for (const auto& [uri, _] : last_published_diagnostics) {
            uris.push_back(uri);
        }
        for (const auto& [uri, _] : open_documents) {
            uris.push_back(uri);
        }
        std::ranges::sort(uris);
        uris.erase(std::ranges::unique(uris).begin(), uris.end());

        std::vector<std::string> outgoing;
        for (const auto& uri : uris) {
            PublishDiagnosticsParams params{
                .uri = uri,
                .diagnostics = merged_diagnostics.contains(uri)
                                   ? merged_diagnostics[uri]
                                   : std::vector<LSPDiagnostic>{},
                .version = std::nullopt,
            };
            if (const auto it = open_documents.find(uri);
                it != open_documents.end()) {
                params.version = it->second.version;
            }
            if (auto notification = make_notification(
                    "textDocument/publishDiagnostics", params);
                notification.has_value()) {
                outgoing.push_back(std::move(notification.value()));
            }
        }

        last_published_diagnostics = std::move(merged_diagnostics);
        return outgoing;
    }

    [[nodiscard]] auto hoverFor(const TextDocumentPositionParams& params) const
        -> std::optional<Hover> {
        const auto it = snapshots.find(params.textDocument.uri);
        if (it == snapshots.end() || it->second.source == nullptr) {
            return std::nullopt;
        }

        const auto& snapshot = it->second;
        const auto query = queryForHover(snapshot, params.position);
        if (!query.has_value()) {
            return std::nullopt;
        }
        return build_hover(snapshot, *snapshot.source, *query);
    }

    [[nodiscard]] auto
    definitionFor(const TextDocumentPositionParams& params) const
        -> std::optional<Location> {
        const auto it = snapshots.find(params.textDocument.uri);
        if (it == snapshots.end() || it->second.source == nullptr) {
            return std::nullopt;
        }

        const auto& snapshot = it->second;
        const auto query = queryForSymbol(snapshot, params.position);
        if (!query.has_value()) {
            return std::nullopt;
        }
        return build_definition_location(snapshot.package, *query);
    }

    [[nodiscard]] auto renameFor(const RenameParams& params) const
        -> std::optional<WorkspaceEdit> {
        const auto it = snapshots.find(params.textDocument.uri);
        if (it == snapshots.end() || it->second.source == nullptr) {
            return std::nullopt;
        }

        const auto& snapshot = it->second;
        const auto query = queryForSymbol(snapshot, params.position);
        if (!query.has_value() || !query->symbol.has_value() ||
            !is_symbol_renamable(query->symbol->kind)) {
            return std::nullopt;
        }

        const auto target_declaration_range =
            redirected_declaration_range(snapshot.package, *query->symbol);
        if (!target_declaration_range.has_value()) {
            return std::nullopt;
        }
        return build_workspace_edit(snapshot, *target_declaration_range,
                                    params.newName);
    }

    [[nodiscard]] auto
    semanticTokensFor(const SemanticTokensParams& params) const
        -> SemanticTokens {
        const auto it = snapshots.find(params.textDocument.uri);
        if (it == snapshots.end() || it->second.source == nullptr) {
            return {};
        }
        return build_semantic_tokens(it->second, *it->second.source);
    }

    std::unordered_map<std::string, OpenDocument> open_documents;
    std::unordered_map<std::string, DocumentSnapshot> snapshots;
    std::unordered_map<std::string, std::vector<LSPDiagnostic>>
        last_published_diagnostics;
    bool shutdown_requested = false;
    bool exit_requested = false;
};

} // namespace lsp_impl

auto LanguageServer::run(std::istream& input, std::ostream& output) -> int {
    lsp_impl::LanguageServerState server;
    while (!server.exitRequested()) {
        const auto body = lsp_impl::read_next_message(input);
        if (!body.has_value()) {
            break;
        }
        for (auto& response : server.handle(*body)) {
            lsp_impl::write_message(output, response);
        }
    }
    return server.exitCode();
}

} // namespace sc

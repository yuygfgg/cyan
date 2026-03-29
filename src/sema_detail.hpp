#pragma once

#include "sc/sema.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sc::detail {

inline auto make_error(std::string message, SourceRange range)
    -> std::expected<void, Diagnostic> {
    return std::unexpected(Diagnostic(std::move(message), range));
}

template <typename T>
auto unexpected_result(std::string message, SourceRange range)
    -> std::expected<T, Diagnostic> {
    return std::unexpected(Diagnostic(std::move(message), range));
}

template <typename... Ts> struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

struct ScopedModule {
    const ast::Module*& slot;
    const ast::Module* saved_value;

    ScopedModule(const ast::Module*& target_slot, const ast::Module* next_value)
        : slot(target_slot), saved_value(target_slot) {
        this->slot = next_value;
    }

    ~ScopedModule() { slot = saved_value; }
};

inline constexpr auto INDEX_FIELD_SENTINEL =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr auto ENUM_PAYLOAD_SENTINEL =
    std::numeric_limits<std::uint32_t>::max() - 1;

auto is_path_prefix(const std::vector<std::uint32_t>& prefix,
                    const std::vector<std::uint32_t>& path) -> bool;
auto mangle_module_name(std::string_view module_name) -> std::string;
auto make_decl_linkage_name(const ast::Module& module, std::string_view name)
    -> std::string;
auto use_qualified_linkage(const ast::Package& package,
                           const ast::Module& module) -> bool;
auto make_readable_type_name(const std::string& base_name,
                             const std::vector<const Type*>& type_arguments,
                             const TypeContext& types) -> std::string;
auto is_same_or_subplace(const ast::ResolvedPlace& candidate,
                         const ast::ResolvedPlace& base) -> bool;
auto make_impl_key(std::string_view interface_name, std::string_view type_name)
    -> std::string;
auto is_borrow_like_type(const Type* type) -> bool;
auto is_view_like_type(const TypeContext& types, const Type* type) -> bool;
auto same_concrete_base_type(const TypeContext& types, const Type* lhs,
                             const Type* rhs) -> bool;
auto can_add_const_in_object_graph(const TypeContext& types, const Type* source,
                                   const Type* target) -> bool;
auto can_convert_pointer_value(const TypeContext& types, const Type* source,
                               const Type* target) -> bool;
auto can_consume_value_type(const TypeContext& types, const Type* source,
                            const Type* target) -> bool;
auto type_syntax_contains_name(const ast::TypeSyntax& type,
                               std::string_view name) -> bool;
auto describe_type_syntax(const ast::TypeSyntax& type) -> std::string;
auto impl_target_group_key(const ast::TypeSyntax& type) -> std::string;
auto impl_target_group_key(const TypeContext& types, const Type* type)
    -> std::string;
auto mangle_type_syntax(const ast::TypeSyntax& type) -> std::string;
auto mangle_type(const Type* type) -> std::string;
auto mangle_instantiation_name(const std::string& base_name,
                               const std::vector<const Type*>& type_arguments)
    -> std::string;
auto make_type_syntax_from_type(const Type* type) -> ast::TypeSyntaxPtr;
auto clone_type_syntax(
    const ast::TypeSyntax& type,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::TypeSyntaxPtr;
auto type_syntax_mentions_parameters(
    const ast::TypeSyntax& type,
    const std::vector<std::string>& type_parameters) -> bool;
auto clone_expr(
    const ast::Expr& expr,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::ExprPtr;
auto clone_stmt(
    const ast::Stmt& stmt,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::StmtPtr;
auto clone_block(
    const ast::Block& block,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::BlockPtr;

} // namespace sc::detail

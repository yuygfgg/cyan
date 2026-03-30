#include "sema_detail.hpp"

namespace cyan::detail {

auto is_path_prefix(const std::vector<std::uint32_t>& prefix,
                    const std::vector<std::uint32_t>& path) -> bool {
    if (prefix.size() > path.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (prefix[index] != path[index]) {
            return false;
        }
    }
    return true;
}

auto mangle_module_name(std::string_view module_name) -> std::string {
    std::string mangled;
    mangled.reserve(module_name.size());
    for (const auto ch : module_name) {
        mangled.push_back(ch == '.' ? '$' : ch);
    }
    return mangled;
}

auto make_decl_linkage_name(const ast::Module& module, std::string_view name)
    -> std::string {
    return "$mod$" + mangle_module_name(module.module_name) + "$" +
           std::string(name);
}

auto use_qualified_linkage(const ast::Package& package,
                           const ast::Module& module) -> bool {
    return package.entry_module != &module;
}

auto make_readable_type_name(const std::string& base_name,
                             const std::vector<const Type*>& type_arguments,
                             const TypeContext& types) -> std::string {
    if (type_arguments.empty()) {
        return base_name;
    }

    std::ostringstream stream;
    stream << base_name << '<';
    for (std::size_t index = 0; index < type_arguments.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }
        stream << types.describe(type_arguments[index]);
    }
    stream << '>';
    return stream.str();
}

auto is_same_or_subplace(const ast::ResolvedPlace& candidate,
                         const ast::ResolvedPlace& base) -> bool {
    if (candidate.root_id != base.root_id ||
        candidate.is_external != base.is_external ||
        candidate.fields.size() < base.fields.size()) {
        return false;
    }

    for (std::size_t index = 0; index < base.fields.size(); ++index) {
        if (candidate.fields[index] != base.fields[index]) {
            return false;
        }
    }
    return true;
}

auto make_impl_key(std::string_view interface_name, std::string_view type_name)
    -> std::string {
    return std::string(interface_name) + ":" + std::string(type_name);
}

auto is_borrow_like_type(const Type* type) -> bool {
    return type != nullptr && (type->kind == TypeKind::Borrow ||
                               type->kind == TypeKind::Interface);
}

auto is_view_like_type(const TypeContext& types, const Type* type) -> bool {
    if (type == nullptr) {
        return false;
    }
    type = types.unqualify(type);
    return is_borrow_like_type(type) || type->kind == TypeKind::Slice;
}

auto is_direct_shared_view_slice(const TypeContext& types, const Type* type)
    -> bool {
    if (type == nullptr) {
        return false;
    }
    type = types.unqualify(type);
    if (type->kind != TypeKind::Slice || type->element_type == nullptr) {
        return false;
    }
    const auto* element = types.unqualify(type->element_type);
    return is_borrow_like_type(element) && !element->is_mut;
}

auto same_concrete_base_type(const TypeContext& types, const Type* lhs,
                             const Type* rhs) -> bool {
    lhs = types.unqualify(lhs);
    rhs = types.unqualify(rhs);
    if (lhs->kind != rhs->kind || lhs->is_mut != rhs->is_mut ||
        lhs->array_size != rhs->array_size) {
        return false;
    }

    switch (lhs->kind) {
    case TypeKind::Void:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
        return true;
    case TypeKind::Struct:
        return lhs->struct_decl == rhs->struct_decl;
    case TypeKind::Enum:
        return lhs->enum_decl == rhs->enum_decl;
    case TypeKind::Interface:
        return lhs->interface_decl == rhs->interface_decl;
    case TypeKind::Borrow:
    case TypeKind::Pointer:
    case TypeKind::Slice:
    case TypeKind::Array:
        return false;
    }
    return false;
}

auto can_add_const_in_object_graph(const TypeContext& types, const Type* source,
                                   const Type* target) -> bool {
    const auto* source_base = types.unqualify(source);
    const auto* target_base = types.unqualify(target);
    if (source_base->kind != target_base->kind ||
        source_base->is_mut != target_base->is_mut ||
        source_base->array_size != target_base->array_size) {
        return false;
    }
    if (source->is_const && !target->is_const) {
        return false;
    }

    switch (source_base->kind) {
    case TypeKind::Void:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Interface:
        return same_concrete_base_type(types, source, target);
    case TypeKind::Array:
        return can_add_const_in_object_graph(types, source_base->element_type,
                                             target_base->element_type);
    case TypeKind::Slice:
        return can_add_const_in_object_graph(types, source_base->element_type,
                                             target_base->element_type);
    case TypeKind::Borrow:
    case TypeKind::Pointer:
        return types.sameIgnoringTopLevelConst(source, target);
    }
    return false;
}

auto can_convert_pointer_value(const TypeContext& types, const Type* source,
                               const Type* target) -> bool {
    const auto* source_base = types.unqualify(source);
    const auto* target_base = types.unqualify(target);
    if (source_base->kind != TypeKind::Pointer ||
        target_base->kind != TypeKind::Pointer) {
        return false;
    }
    return can_add_const_in_object_graph(types, source_base->element_type,
                                         target_base->element_type);
}

auto can_consume_value_type(const TypeContext& types, const Type* source,
                            const Type* target) -> bool {
    if (types.isSame(source, target)) {
        return true;
    }

    const auto* source_base = types.unqualify(source);
    const auto* target_base = types.unqualify(target);
    if (source_base->kind != target_base->kind) {
        return false;
    }

    switch (source_base->kind) {
    case TypeKind::Void:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
    case TypeKind::Struct:
    case TypeKind::Enum:
        return same_concrete_base_type(types, source, target);
    case TypeKind::Array:
        return source_base->array_size == target_base->array_size &&
               can_consume_value_type(types, source_base->element_type,
                                      target_base->element_type);
    case TypeKind::Slice:
        return can_add_const_in_object_graph(types, source_base->element_type,
                                             target_base->element_type);
    case TypeKind::Pointer:
        return can_convert_pointer_value(types, source, target);
    case TypeKind::Borrow:
    case TypeKind::Interface:
        return false;
    }
    return false;
}

auto type_syntax_contains_name(const ast::TypeSyntax& type,
                               std::string_view name) -> bool {
    if (type.kind == ast::TypeSyntax::Kind::Named && type.name == name) {
        return true;
    }
    if (type.element_type != nullptr &&
        type_syntax_contains_name(*type.element_type, name)) {
        return true;
    }
    return std::ranges::any_of(
        type.type_arguments, [&](const auto& type_argument) {
            return type_syntax_contains_name(*type_argument, name);
        });
}

auto describe_type_syntax(const ast::TypeSyntax& type) -> std::string {
    auto describe_unqualified = [&](const ast::TypeSyntax& inner,
                                    const auto& self) -> std::string {
        switch (inner.kind) {
        case ast::TypeSyntax::Kind::Named: {
            if (inner.type_arguments.empty()) {
                return inner.name;
            }

            std::ostringstream stream;
            stream << inner.name << '<';
            for (std::size_t index = 0; index < inner.type_arguments.size();
                 ++index) {
                if (index != 0) {
                    stream << ", ";
                }
                stream << self(*inner.type_arguments[index], self);
            }
            stream << '>';
            return stream.str();
        }
        case ast::TypeSyntax::Kind::Borrow:
            return std::string(inner.is_mut ? "&mut " : "&") +
                   self(*inner.element_type, self);
        case ast::TypeSyntax::Kind::Slice:
            return "[]" + self(*inner.element_type, self);
        case ast::TypeSyntax::Kind::Pointer:
            return self(*inner.element_type, self) + '*';
        case ast::TypeSyntax::Kind::Array: {
            std::ostringstream stream;
            stream << self(*inner.element_type, self) << '[' << inner.array_size
                   << ']';
            return stream.str();
        }
        }
        return "<invalid>";
    };

    const auto described = describe_unqualified(type, describe_unqualified);
    return type.is_const ? "const " + described : described;
}

auto impl_target_group_key(const ast::TypeSyntax& type) -> std::string {
    switch (type.kind) {
    case ast::TypeSyntax::Kind::Named:
        return "named:" + type.name;
    case ast::TypeSyntax::Kind::Borrow:
        return "borrow";
    case ast::TypeSyntax::Kind::Slice:
        return "slice";
    case ast::TypeSyntax::Kind::Pointer:
        return "pointer";
    case ast::TypeSyntax::Kind::Array:
        return "array";
    }
    return "invalid";
}

auto mangle_type_syntax(const ast::TypeSyntax& type) -> std::string {
    auto mangle_unqualified = [&](const ast::TypeSyntax& inner,
                                  const auto& self) -> std::string {
        switch (inner.kind) {
        case ast::TypeSyntax::Kind::Named: {
            if (inner.type_arguments.empty()) {
                return inner.name;
            }
            std::ostringstream stream;
            stream << inner.name << "$LT$";
            for (std::size_t index = 0; index < inner.type_arguments.size();
                 ++index) {
                if (index != 0) {
                    stream << "$COMMA$";
                }
                stream << self(*inner.type_arguments[index], self);
            }
            stream << "$GT$";
            return stream.str();
        }
        case ast::TypeSyntax::Kind::Borrow:
            return std::string(inner.is_mut ? "mutref$LT$" : "ref$LT$") +
                   self(*inner.element_type, self) + "$GT";
        case ast::TypeSyntax::Kind::Slice:
            return "slice$LT$" + self(*inner.element_type, self) + "$GT";
        case ast::TypeSyntax::Kind::Pointer:
            return "ptr$LT$" + self(*inner.element_type, self) + "$GT";
        case ast::TypeSyntax::Kind::Array: {
            std::ostringstream stream;
            stream << "arr$LT$" << inner.array_size << "$COMMA$"
                   << self(*inner.element_type, self) << "$GT";
            return stream.str();
        }
        }
        return "invalid";
    };

    auto mangled = mangle_unqualified(type, mangle_unqualified);
    if (type.is_const) {
        return "const$LT$" + mangled + "$GT";
    }
    return mangled;
}

auto mangle_type(const Type* type) -> std::string {
    auto mangle_unqualified = [&](const Type* inner,
                                  const auto& self) -> std::string {
        switch (inner->kind) {
        case TypeKind::Void:
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::Char:
        case TypeKind::Bool:
        case TypeKind::Struct:
        case TypeKind::Enum:
            return inner->linkage_name;
        case TypeKind::Interface:
            return "iface$LT$" + inner->linkage_name + "$GT";
        case TypeKind::Borrow:
            return (inner->is_mut ? "mutref$LT$" : "ref$LT$") +
                   self(inner->element_type, self) + "$GT";
        case TypeKind::Slice:
            return "slice$LT$" + self(inner->element_type, self) + "$GT";
        case TypeKind::Pointer:
            return "ptr$LT$" + self(inner->element_type, self) + "$GT";
        case TypeKind::Array: {
            std::ostringstream stream;
            stream << "arr$LT$" << inner->array_size << "$COMMA$"
                   << self(inner->element_type, self) << "$GT";
            return stream.str();
        }
        }
        return "invalid";
    };

    if (type->is_const) {
        return "const$LT$" + mangle_unqualified(type, mangle_unqualified) +
               "$GT";
    }
    return mangle_unqualified(type, mangle_unqualified);
}

auto impl_target_group_key(const TypeContext& types, const Type* type)
    -> std::string {
    if (type == nullptr) {
        return "invalid";
    }

    type = type->kind == TypeKind::Borrow ? type->element_type : type;
    type = type == nullptr ? nullptr : types.unqualify(type);
    if (type == nullptr) {
        return "invalid";
    }

    switch (type->kind) {
    case TypeKind::Void:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
        return "named:" + type->name;
    case TypeKind::Struct:
        if (type->struct_decl != nullptr &&
            type->struct_decl->template_decl != nullptr) {
            return "named:" + type->struct_decl->template_decl->name;
        }
        return "named:" + type->name;
    case TypeKind::Enum:
        if (type->enum_decl != nullptr &&
            type->enum_decl->template_decl != nullptr) {
            return "named:" + type->enum_decl->template_decl->name;
        }
        return "named:" + type->name;
    case TypeKind::Interface:
        return "interface";
    case TypeKind::Borrow:
        return "borrow";
    case TypeKind::Pointer:
        return "pointer";
    case TypeKind::Slice:
        return "slice";
    case TypeKind::Array:
        return "array";
    }
    return "invalid";
}

auto mangle_instantiation_name(const std::string& base_name,
                               const std::vector<const Type*>& type_arguments)
    -> std::string {
    if (type_arguments.empty()) {
        return base_name;
    }

    std::ostringstream stream;
    stream << base_name << "$LT$";
    for (std::size_t index = 0; index < type_arguments.size(); ++index) {
        if (index != 0) {
            stream << "$COMMA$";
        }
        stream << mangle_type(type_arguments[index]);
    }
    stream << "$GT$";
    return stream.str();
}

auto make_type_syntax_from_type(const Type* type) -> ast::TypeSyntaxPtr {
    auto syntax = std::make_unique<ast::TypeSyntax>();
    syntax->is_const = type->is_const;
    switch (type->kind) {
    case TypeKind::Void:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
    case TypeKind::Struct:
    case TypeKind::Enum:
        syntax->kind = ast::TypeSyntax::Kind::Named;
        syntax->name = type->name;
        break;
    case TypeKind::Interface:
        syntax->kind = ast::TypeSyntax::Kind::Borrow;
        syntax->element_type = std::make_unique<ast::TypeSyntax>();
        syntax->element_type->kind = ast::TypeSyntax::Kind::Named;
        syntax->element_type->name = type->name;
        break;
    case TypeKind::Borrow:
        syntax->kind = ast::TypeSyntax::Kind::Borrow;
        syntax->is_mut = type->is_mut;
        syntax->element_type = make_type_syntax_from_type(type->element_type);
        break;
    case TypeKind::Slice:
        syntax->kind = ast::TypeSyntax::Kind::Slice;
        syntax->element_type = make_type_syntax_from_type(type->element_type);
        break;
    case TypeKind::Pointer:
        syntax->kind = ast::TypeSyntax::Kind::Pointer;
        syntax->element_type = make_type_syntax_from_type(type->element_type);
        break;
    case TypeKind::Array:
        syntax->kind = ast::TypeSyntax::Kind::Array;
        syntax->array_size = type->array_size;
        syntax->element_type = make_type_syntax_from_type(type->element_type);
        break;
    }
    return syntax;
}

auto clone_type_syntax(
    const ast::TypeSyntax& type,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::TypeSyntaxPtr {
    if (type.kind == ast::TypeSyntax::Kind::Named &&
        type.type_arguments.empty()) {
        if (const auto binding_it = type_bindings.find(type.name);
            binding_it != type_bindings.end()) {
            return make_type_syntax_from_type(binding_it->second);
        }
    }

    auto clone = std::make_unique<ast::TypeSyntax>();
    clone->range = type.range;
    clone->kind = type.kind;
    clone->name = type.name;
    clone->name_range = type.name_range;
    clone->is_mut = type.is_mut;
    clone->is_const = type.is_const;
    clone->array_size = type.array_size;
    if (type.element_type != nullptr) {
        clone->element_type =
            clone_type_syntax(*type.element_type, type_bindings);
    }
    clone->type_arguments.reserve(type.type_arguments.size());
    for (const auto& type_argument : type.type_arguments) {
        clone->type_arguments.push_back(
            clone_type_syntax(*type_argument, type_bindings));
    }
    return clone;
}

auto type_syntax_mentions_parameters(
    const ast::TypeSyntax& type,
    const std::vector<std::string>& type_parameters) -> bool {
    if (type.kind == ast::TypeSyntax::Kind::Named &&
        type.type_arguments.empty() &&
        std::ranges::find(type_parameters, type.name) !=
            type_parameters.end()) {
        return true;
    }

    if (type.element_type != nullptr &&
        type_syntax_mentions_parameters(*type.element_type, type_parameters)) {
        return true;
    }

    return std::ranges::any_of(type.type_arguments,
                               [&](const auto& type_argument) {
                                   return type_syntax_mentions_parameters(
                                       *type_argument, type_parameters);
                               });
}

auto clone_block(
    const ast::Block& block,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::BlockPtr {
    auto clone = std::make_unique<ast::Block>();
    clone->range = block.range;
    for (const auto& statement : block.statements) {
        clone->statements.push_back(clone_stmt(*statement, type_bindings));
    }
    return clone;
}

auto clone_expr(
    const ast::Expr& expr,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::ExprPtr {
    auto clone = std::make_unique<ast::Expr>();
    clone->range = expr.range;
    clone->node = std::visit(
        Overloaded{
            [&](const ast::IntegerLiteralExpr& literal) -> ast::Expr::Variant {
                return literal;
            },
            [&](const ast::FloatLiteralExpr& literal) -> ast::Expr::Variant {
                return literal;
            },
            [&](const ast::CharLiteralExpr& literal) -> ast::Expr::Variant {
                return literal;
            },
            [&](const ast::BoolLiteralExpr& literal) -> ast::Expr::Variant {
                return literal;
            },
            [&](const ast::StringLiteralExpr& literal) -> ast::Expr::Variant {
                return literal;
            },
            [&](const ast::NameExpr& name) -> ast::Expr::Variant {
                ast::NameExpr clone_name;
                clone_name.name = name.name;
                clone_name.name_range = name.name_range;
                return clone_name;
            },
            [&](const ast::UnaryExpr& unary) -> ast::Expr::Variant {
                return ast::UnaryExpr{
                    .op = unary.op,
                    .operand = clone_expr(*unary.operand, type_bindings)};
            },
            [&](const ast::BinaryExpr& binary) -> ast::Expr::Variant {
                return ast::BinaryExpr{
                    .op = binary.op,
                    .lhs = clone_expr(*binary.lhs, type_bindings),
                    .rhs = clone_expr(*binary.rhs, type_bindings)};
            },
            [&](const ast::CallExpr& call) -> ast::Expr::Variant {
                ast::CallExpr clone_call;
                clone_call.callee = call.callee;
                clone_call.callee_range = call.callee_range;
                clone_call.builtin_kind = call.builtin_kind;
                for (const auto& argument : call.arguments) {
                    clone_call.arguments.push_back(
                        clone_expr(*argument, type_bindings));
                }
                return clone_call;
            },
            [&](const ast::MemberExpr& member) -> ast::Expr::Variant {
                ast::MemberExpr clone_member;
                clone_member.base = clone_expr(*member.base, type_bindings);
                clone_member.field_name = member.field_name;
                clone_member.field_range = member.field_range;
                return clone_member;
            },
            [&](const ast::IndexExpr& index) -> ast::Expr::Variant {
                return ast::IndexExpr{
                    .base = clone_expr(*index.base, type_bindings),
                    .index = clone_expr(*index.index, type_bindings)};
            },
            [&](const ast::InitListExpr& init_list) -> ast::Expr::Variant {
                ast::InitListExpr clone_init_list;
                for (const auto& element : init_list.elements) {
                    clone_init_list.elements.push_back(
                        clone_expr(*element, type_bindings));
                }
                return clone_init_list;
            },
            [&](const ast::ArrayLiteralExpr& array_literal)
                -> ast::Expr::Variant {
                ast::ArrayLiteralExpr clone_array_literal;
                for (const auto& element : array_literal.elements) {
                    clone_array_literal.elements.push_back(
                        clone_expr(*element, type_bindings));
                }
                return clone_array_literal;
            },
            [&](const ast::CastExpr& cast_expr) -> ast::Expr::Variant {
                return ast::CastExpr{
                    .operand = clone_expr(*cast_expr.operand, type_bindings),
                    .target_type = clone_type_syntax(*cast_expr.target_type,
                                                     type_bindings),
                    .cast_kind = ast::CastKind::None};
            },
            [&](const ast::SizeofExpr& sizeof_expr) -> ast::Expr::Variant {
                return ast::SizeofExpr{
                    .type = clone_type_syntax(*sizeof_expr.type, type_bindings),
                    .operand_type = nullptr};
            },
        },
        expr.node);
    return clone;
}

auto clone_stmt(
    const ast::Stmt& stmt,
    const std::unordered_map<std::string, const Type*>& type_bindings)
    -> ast::StmtPtr {
    auto clone = std::make_unique<ast::Stmt>();
    clone->range = stmt.range;
    clone->node = std::visit(
        Overloaded{
            [&](const ast::VarDeclStmt& var_decl) -> ast::Stmt::Variant {
                ast::VarDeclStmt clone_var_decl;
                clone_var_decl.type =
                    clone_type_syntax(*var_decl.type, type_bindings);
                clone_var_decl.name = var_decl.name;
                clone_var_decl.name_range = var_decl.name_range;
                if (var_decl.initializer != nullptr) {
                    clone_var_decl.initializer =
                        clone_expr(*var_decl.initializer, type_bindings);
                }
                return clone_var_decl;
            },
            [&](const ast::ExprStmt& expr_stmt) -> ast::Stmt::Variant {
                return ast::ExprStmt{
                    clone_expr(*expr_stmt.expr, type_bindings)};
            },
            [&](const ast::AssignStmt& assign) -> ast::Stmt::Variant {
                return ast::AssignStmt{
                    .target = clone_expr(*assign.target, type_bindings),
                    .value = clone_expr(*assign.value, type_bindings)};
            },
            [&](const ast::UpdateStmt& update) -> ast::Stmt::Variant {
                return ast::UpdateStmt{
                    .target = clone_expr(*update.target, type_bindings),
                    .is_increment = update.is_increment};
            },
            [&](const ast::ReturnStmt& ret) -> ast::Stmt::Variant {
                ast::ReturnStmt clone_return;
                if (ret.value != nullptr) {
                    clone_return.value = clone_expr(*ret.value, type_bindings);
                }
                return clone_return;
            },
            [&](const ast::DropStmt& drop_stmt) -> ast::Stmt::Variant {
                return ast::DropStmt{
                    clone_expr(*drop_stmt.value, type_bindings)};
            },
            [&](const ast::IfStmt& if_stmt) -> ast::Stmt::Variant {
                ast::IfStmt clone_if;
                clone_if.condition =
                    clone_expr(*if_stmt.condition, type_bindings);
                clone_if.then_block =
                    clone_block(*if_stmt.then_block, type_bindings);
                if (if_stmt.else_block != nullptr) {
                    clone_if.else_block =
                        clone_block(*if_stmt.else_block, type_bindings);
                }
                return clone_if;
            },
            [&](const ast::WhileStmt& while_stmt) -> ast::Stmt::Variant {
                return ast::WhileStmt{
                    .condition =
                        clone_expr(*while_stmt.condition, type_bindings),
                    .body = clone_block(*while_stmt.body, type_bindings)};
            },
            [&](const ast::ForStmt& for_stmt) -> ast::Stmt::Variant {
                ast::ForStmt clone_for;
                if (for_stmt.initializer != nullptr) {
                    clone_for.initializer =
                        clone_stmt(*for_stmt.initializer, type_bindings);
                }
                if (for_stmt.condition != nullptr) {
                    clone_for.condition =
                        clone_expr(*for_stmt.condition, type_bindings);
                }
                if (for_stmt.step != nullptr) {
                    clone_for.step = clone_stmt(*for_stmt.step, type_bindings);
                }
                clone_for.body = clone_block(*for_stmt.body, type_bindings);
                return clone_for;
            },
            [&](const ast::BreakStmt&) -> ast::Stmt::Variant {
                return ast::BreakStmt{};
            },
            [&](const ast::ContinueStmt&) -> ast::Stmt::Variant {
                return ast::ContinueStmt{};
            },
            [&](const ast::UncheckedStmt& unchecked_stmt)
                -> ast::Stmt::Variant {
                return ast::UncheckedStmt{
                    clone_block(*unchecked_stmt.body, type_bindings)};
            },
            [&](const ast::SwitchStmt& switch_stmt) -> ast::Stmt::Variant {
                ast::SwitchStmt clone_switch;
                clone_switch.scrutinee =
                    clone_expr(*switch_stmt.scrutinee, type_bindings);
                clone_switch.match_kind = switch_stmt.match_kind;
                for (const auto& switch_case : switch_stmt.cases) {
                    ast::SwitchCase clone_case;
                    clone_case.range = switch_case.range;
                    clone_case.is_default = switch_case.is_default;
                    clone_case.variant_name = switch_case.variant_name;
                    clone_case.variant_name_range =
                        switch_case.variant_name_range;
                    clone_case.binding_name = switch_case.binding_name;
                    clone_case.binding_name_range =
                        switch_case.binding_name_range;
                    clone_case.body =
                        clone_block(*switch_case.body, type_bindings);
                    clone_switch.cases.push_back(std::move(clone_case));
                }
                return clone_switch;
            },
            [&](const ast::Block& block) -> ast::Stmt::Variant {
                ast::Block clone_block_value;
                clone_block_value.range = block.range;
                for (const auto& statement : block.statements) {
                    clone_block_value.statements.push_back(
                        clone_stmt(*statement, type_bindings));
                }
                return clone_block_value;
            },
        },
        stmt.node);
    return clone;
}

} // namespace cyan::detail

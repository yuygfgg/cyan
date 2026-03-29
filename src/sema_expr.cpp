#include "sema_detail.hpp"

namespace sc {

using namespace detail;

auto SemanticAnalyzer::analyzeExpr(FunctionState& state, ast::Expr& expr,
                                   const Type* expected_type)
    -> std::expected<const Type*, Diagnostic> {
    expr.interface_source_type = nullptr;
    expr.interface_impl = nullptr;
    expr.slice_source_type = nullptr;
    expr.slice_source_place.reset();
    if (expected_type != nullptr &&
        expected_type->kind == TypeKind::Interface) {
        auto coerced = coerceExprToInterface(state, expr, expected_type);
        if (!coerced) {
            return std::unexpected(coerced.error());
        }
        return expected_type;
    }
    if (expected_type != nullptr &&
        types.unqualify(expected_type)->kind == TypeKind::Slice &&
        !std::holds_alternative<ast::ArrayLiteralExpr>(expr.node)) {
        auto coerced = coerceExprToSlice(state, expr, expected_type);
        if (!coerced) {
            return std::unexpected(coerced.error());
        }
        return expr.resolved_type;
    }
    return std::visit(
        Overloaded{
            [&](ast::IntegerLiteralExpr&)
                -> std::expected<const Type*, Diagnostic> {
                expr.resolved_type = types.intType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::FloatLiteralExpr&)
                -> std::expected<const Type*, Diagnostic> {
                expr.resolved_type = types.floatType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::CharLiteralExpr&)
                -> std::expected<const Type*, Diagnostic> {
                expr.resolved_type = types.charType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::BoolLiteralExpr&)
                -> std::expected<const Type*, Diagnostic> {
                expr.resolved_type = types.boolType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::StringLiteralExpr& literal)
                -> std::expected<const Type*, Diagnostic> {
                const auto* const_char = types.getConst(types.charType());
                if (expected_type != nullptr &&
                    expected_type->kind == TypeKind::Pointer &&
                    can_add_const_in_object_graph(
                        types, const_char, expected_type->element_type)) {
                    expr.resolved_type = expected_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                expr.resolved_type =
                    types.getArray(const_char, literal.value.size() + 1);
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::NameExpr& name) -> std::expected<const Type*, Diagnostic> {
                if (const auto local_index = lookupLocal(state, name.name);
                    local_index.has_value()) {
                    auto& local = state.locals[*local_index];
                    if (local.status != LocalState::Status::Live) {
                        if (local.status == LocalState::Status::Moved &&
                            expr.resolved_type != nullptr &&
                            expr.resolved_place.has_value() &&
                            expr.resolved_place->root_id == local.unique_id &&
                            !expr.resolved_place->is_external &&
                            name.local_id == local.unique_id) {
                            return expr.resolved_type;
                        }
                        if (local.status == LocalState::Status::Uninitialized &&
                            state.relaxed_place_resolution_depth > 0) {
                            name.local_id = local.unique_id;
                            expr.resolved_type = local.type;
                            expr.resolved_place = localPlace(local.unique_id);
                            return expr.resolved_type;
                        }
                        const auto message =
                            local.status == LocalState::Status::Moved
                                ? "use of moved local '" + local.name + "'"
                                : "use of uninitialized local '" + local.name +
                                      "'";
                        return unexpected_result<const Type*>(message,
                                                              expr.range);
                    }
                    name.local_id = local.unique_id;
                    if (typeContainsViews(local.type)) {
                        auto view_live = ensureViewSubtreeLive(
                            state, localPlace(local.unique_id), local.type,
                            expr.range);
                        if (!view_live) {
                            return std::unexpected(view_live.error());
                        }
                    } else if (types.unqualify(local.type)->kind ==
                                   TypeKind::Slice &&
                               local.borrow_origin.has_value() &&
                               !local.borrow_origin->is_external) {
                        const auto source_index =
                            findLocalById(state, local.borrow_origin->root_id);
                        if (source_index.has_value()) {
                            const auto& source_local =
                                state.locals[*source_index];
                            if (!source_local.in_scope ||
                                source_local.status !=
                                    LocalState::Status::Live) {
                                return unexpected_result<const Type*>(
                                    "slice source is no longer live",
                                    expr.range);
                            }
                        }
                    }
                    expr.resolved_type = local.type;
                    expr.resolved_place = localPlace(local.unique_id);
                    return expr.resolved_type;
                }

                const auto& scope = visibleScopeFor(*active_module);
                if (const auto it = scope.functions.find(name.name);
                    it != scope.functions.end()) {
                    name.function = it->second;
                    expr.resolved_type = nullptr;
                    expr.resolved_place.reset();
                    return unexpected_result<const Type*>(
                        "function name can only appear in a direct call "
                        "expression",
                        expr.range);
                }
                if (scope.function_templates.contains(name.name)) {
                    return unexpected_result<const Type*>(
                        "generic function name can only appear in a direct "
                        "call expression",
                        expr.range);
                }
                if (scope.variants.contains(name.name) ||
                    scope.template_variants.contains(name.name)) {
                    return unexpected_result<const Type*>(
                        "enum variant names can only appear in constructors or "
                        "switch cases",
                        expr.range);
                }
                return unexpected_result<const Type*>(
                    "unknown identifier '" + name.name + "'", expr.range);
            },
            [&](ast::UnaryExpr& unary)
                -> std::expected<const Type*, Diagnostic> {
                switch (unary.op) {
                case ast::UnaryOp::Negate: {
                    auto operand_type = analyzeExpr(state, *unary.operand);
                    if (!operand_type) {
                        return std::unexpected(operand_type.error());
                    }
                    const auto* value_type = types.unqualify(*operand_type);
                    if (value_type != types.intType() &&
                        value_type != types.floatType()) {
                        return unexpected_result<const Type*>(
                            "unary '-' requires an int or float operand",
                            unary.operand->range);
                    }
                    expr.resolved_type = value_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                case ast::UnaryOp::LogicalNot: {
                    auto operand_type = analyzeExpr(state, *unary.operand);
                    if (!operand_type) {
                        return std::unexpected(operand_type.error());
                    }
                    if (types.unqualify(*operand_type) != types.boolType()) {
                        return unexpected_result<const Type*>(
                            "unary '!' requires a bool operand",
                            unary.operand->range);
                    }
                    expr.resolved_type = types.boolType();
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                case ast::UnaryOp::Dereference: {
                    auto operand_type = analyzeExpr(state, *unary.operand);
                    if (!operand_type) {
                        return std::unexpected(operand_type.error());
                    }
                    if ((*operand_type)->kind == TypeKind::Pointer) {
                        if (state.unchecked_depth == 0) {
                            return unexpected_result<const Type*>(
                                "raw pointer dereference is only allowed in "
                                "unchecked blocks",
                                unary.operand->range);
                        }
                        expr.resolved_type = (*operand_type)->element_type;
                        expr.resolved_place =
                            ast::ResolvedPlace{.root_id = next_external_root++,
                                               .is_external = true,
                                               .owner_local_id = std::nullopt,
                                               .fields = {}};
                        return expr.resolved_type;
                    }
                    if ((*operand_type)->kind != TypeKind::Borrow) {
                        return unexpected_result<const Type*>(
                            "only borrow values can be dereferenced in safe "
                            "code",
                            unary.operand->range);
                    }
                    auto source_place =
                        borrowSourcePlace(state, *unary.operand);
                    if (!source_place) {
                        return std::unexpected(source_place.error());
                    }
                    expr.resolved_type = (*operand_type)->element_type;
                    expr.resolved_place = *source_place;
                    if ((*operand_type)->is_mut) {
                        auto source_local_id =
                            borrowSourceLocalId(state, *unary.operand);
                        if (!source_local_id) {
                            return std::unexpected(source_local_id.error());
                        }
                        if (source_local_id->has_value()) {
                            expr.resolved_place->owner_local_id =
                                **source_local_id;
                        }
                    }
                    return expr.resolved_type;
                }
                case ast::UnaryOp::Move: {
                    auto operand_type = analyzeExpr(state, *unary.operand);
                    if (!operand_type) {
                        return std::unexpected(operand_type.error());
                    }
                    expr.resolved_type = *operand_type;
                    expr.resolved_place = unary.operand->resolved_place;
                    return expr.resolved_type;
                }
                case ast::UnaryOp::Borrow:
                case ast::UnaryOp::BorrowMut: {
                    const auto want_mut =
                        unary.op == ast::UnaryOp::BorrowMut ||
                        (expected_type != nullptr &&
                         expected_type->kind == TypeKind::Borrow &&
                         expected_type->is_mut);
                    const auto allow_uninitialized_pointer_source =
                        state.unchecked_depth > 0 && expected_type != nullptr &&
                        expected_type->kind == TypeKind::Pointer && want_mut &&
                        expected_type->element_type != nullptr &&
                        !expected_type->element_type->is_const;
                    if (allow_uninitialized_pointer_source) {
                        state.relaxed_place_resolution_depth++;
                    }
                    auto place = resolvePlace(state, *unary.operand);
                    if (allow_uninitialized_pointer_source) {
                        state.relaxed_place_resolution_depth--;
                    }
                    if (!place) {
                        return std::unexpected(place.error());
                    }
                    if (expected_type != nullptr &&
                        expected_type->kind == TypeKind::Pointer &&
                        !allow_uninitialized_pointer_source &&
                        !place->is_external) {
                        const auto local_index =
                            findLocalById(state, place->root_id);
                        if (local_index.has_value()) {
                            const auto& local = state.locals[*local_index];
                            if (local.status != LocalState::Status::Live) {
                                const auto message =
                                    local.status == LocalState::Status::Moved
                                        ? "use of moved local '" + local.name +
                                              "'"
                                        : "use of uninitialized local '" +
                                              local.name + "'";
                                return unexpected_result<const Type*>(
                                    message, unary.operand->range);
                            }
                        }
                    }
                    if (expected_type != nullptr &&
                        expected_type->kind == TypeKind::Pointer) {
                        if (!can_add_const_in_object_graph(
                                types, unary.operand->resolved_type,
                                expected_type->element_type)) {
                            return unexpected_result<const Type*>(
                                "borrow-to-pointer conversion requires "
                                "matching pointee type",
                                expr.range);
                        }
                        expr.resolved_type = expected_type;
                        expr.resolved_place.reset();
                        return expr.resolved_type;
                    }
                    const auto* borrow_operand_type =
                        unary.operand->resolved_type;
                    if (!want_mut) {
                        borrow_operand_type =
                            types.unqualify(borrow_operand_type);
                    }
                    expr.resolved_type =
                        types.getBorrow(borrow_operand_type, want_mut);
                    expr.resolved_place = *place;
                    return expr.resolved_type;
                }
                }
                return unexpected_result<const Type*>(
                    "invalid unary expression", expr.range);
            },
            [&](ast::BinaryExpr& binary)
                -> std::expected<const Type*, Diagnostic> {
                auto lhs_type = analyzeExpr(state, *binary.lhs);
                if (!lhs_type) {
                    return std::unexpected(lhs_type.error());
                }
                auto rhs_type = analyzeExpr(state, *binary.rhs);
                if (!rhs_type) {
                    return std::unexpected(rhs_type.error());
                }
                const auto* lhs_value_type = types.unqualify(*lhs_type);
                const auto* rhs_value_type = types.unqualify(*rhs_type);

                switch (binary.op) {
                case ast::BinaryOp::Add:
                case ast::BinaryOp::Subtract:
                case ast::BinaryOp::Multiply:
                case ast::BinaryOp::Divide:
                case ast::BinaryOp::Remainder:
                    if (lhs_value_type == types.intType() &&
                        rhs_value_type == types.intType()) {
                        expr.resolved_type = types.intType();
                        break;
                    }
                    if (lhs_value_type == types.floatType() &&
                        rhs_value_type == types.floatType()) {
                        expr.resolved_type = types.floatType();
                        break;
                    }
                    if (state.unchecked_depth > 0 &&
                        lhs_value_type->kind == TypeKind::Pointer &&
                        rhs_value_type == types.intType() &&
                        (types.unqualify(lhs_value_type->element_type) !=
                         types.voidType()) &&
                        (binary.op == ast::BinaryOp::Add ||
                         binary.op == ast::BinaryOp::Subtract)) {
                        expr.resolved_type = lhs_value_type;
                        break;
                    }
                    if (state.unchecked_depth > 0 &&
                        lhs_value_type == types.intType() &&
                        rhs_value_type->kind == TypeKind::Pointer &&
                        (types.unqualify(rhs_value_type->element_type) !=
                         types.voidType()) &&
                        binary.op == ast::BinaryOp::Add) {
                        expr.resolved_type = rhs_value_type;
                        break;
                    }
                    return unexpected_result<const Type*>(
                        "arithmetic operators require int operands, float "
                        "operands, or unchecked pointer arithmetic",
                        expr.range);
                case ast::BinaryOp::Less:
                case ast::BinaryOp::LessEqual:
                case ast::BinaryOp::Greater:
                case ast::BinaryOp::GreaterEqual:
                    if ((lhs_value_type != types.intType() ||
                         rhs_value_type != types.intType()) &&
                        (lhs_value_type != types.floatType() ||
                         rhs_value_type != types.floatType())) {
                        return unexpected_result<const Type*>(
                            "comparison operators require int or float "
                            "operands",
                            expr.range);
                    }
                    expr.resolved_type = types.boolType();
                    break;
                case ast::BinaryOp::Equal:
                case ast::BinaryOp::NotEqual:
                    if (!can_consume_value_type(types, *lhs_type, *rhs_type) &&
                        !can_consume_value_type(types, *rhs_type, *lhs_type)) {
                        return unexpected_result<const Type*>(
                            "equality operands must have the same type",
                            expr.range);
                    }
                    if (!types.isCopy(lhs_value_type)) {
                        return unexpected_result<const Type*>(
                            "equality is only implemented for copy types in "
                            "the core MVP",
                            expr.range);
                    }
                    expr.resolved_type = types.boolType();
                    break;
                case ast::BinaryOp::LogicalAnd:
                case ast::BinaryOp::LogicalOr:
                    if (lhs_value_type != types.boolType() ||
                        rhs_value_type != types.boolType()) {
                        return unexpected_result<const Type*>(
                            "logical operators require bool operands",
                            expr.range);
                    }
                    expr.resolved_type = types.boolType();
                    break;
                }
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::CallExpr& call) {
                return analyzeCall(state, expr, call, expected_type);
            },
            [&](ast::MemberExpr& member) {
                return analyzeMember(state, expr, member);
            },
            [&](ast::IndexExpr& index_expr)
                -> std::expected<const Type*, Diagnostic> {
                auto base_type = analyzeExpr(state, *index_expr.base);
                if (!base_type) {
                    return std::unexpected(base_type.error());
                }
                auto index_type = analyzeExpr(state, *index_expr.index);
                if (!index_type) {
                    return std::unexpected(index_type.error());
                }
                if (types.unqualify(*index_type) != types.intType()) {
                    return unexpected_result<const Type*>(
                        "index expression must have type int",
                        index_expr.index->range);
                }
                const Type* element_type = nullptr;
                const auto* base_value_type = types.unqualify(*base_type);
                if (base_value_type->kind == TypeKind::Array) {
                    element_type = base_value_type->element_type;
                    if ((*base_type)->is_const && !element_type->is_const) {
                        element_type = types.getConst(element_type);
                    }
                } else if (base_value_type->kind == TypeKind::Borrow &&
                           base_value_type->element_type != nullptr &&
                           types.unqualify(base_value_type->element_type)
                                   ->kind == TypeKind::Array) {
                    const auto* array_type_raw = base_value_type->element_type;
                    const auto* array_type = types.unqualify(array_type_raw);
                    element_type = array_type->element_type;
                    if (array_type_raw->is_const && !element_type->is_const) {
                        element_type = types.getConst(element_type);
                    }
                } else if (base_value_type->kind == TypeKind::Pointer) {
                    if (state.unchecked_depth == 0) {
                        return unexpected_result<const Type*>(
                            "pointer indexing is only allowed in unchecked "
                            "blocks",
                            index_expr.base->range);
                    }
                    element_type = base_value_type->element_type;
                } else if (base_value_type->kind == TypeKind::Slice) {
                    element_type = base_value_type->element_type;
                    if (!element_type->is_const) {
                        element_type = types.getConst(element_type);
                    }
                } else {
                    return unexpected_result<const Type*>(
                        "indexing requires an array, slice, array borrow, or "
                        "pointer value",
                        index_expr.base->range);
                }

                expr.resolved_type = element_type;
                if (base_value_type->kind == TypeKind::Slice) {
                    auto source_place =
                        sliceSourcePlace(state, *index_expr.base);
                    if (!source_place) {
                        return std::unexpected(source_place.error());
                    }
                    if (source_place->has_value()) {
                        expr.resolved_place = **source_place;
                    } else {
                        expr.resolved_place =
                            ast::ResolvedPlace{.root_id = next_external_root++,
                                               .is_external = true,
                                               .owner_local_id = std::nullopt,
                                               .fields = {}};
                    }
                    expr.resolved_place->fields.push_back(INDEX_FIELD_SENTINEL);
                } else if (index_expr.base->resolved_place.has_value()) {
                    expr.resolved_place = *index_expr.base->resolved_place;
                    expr.resolved_place->fields.push_back(INDEX_FIELD_SENTINEL);
                } else {
                    expr.resolved_place =
                        ast::ResolvedPlace{.root_id = next_external_root++,
                                           .is_external = true,
                                           .owner_local_id = std::nullopt,
                                           .fields = {}};
                }
                if (base_value_type->kind == TypeKind::Slice &&
                    is_borrow_like_type(types.unqualify(expr.resolved_type))) {
                    auto source_bindings =
                        collectExprViewBindings(state, *index_expr.base);
                    if (!source_bindings) {
                        return std::unexpected(source_bindings.error());
                    }
                    const auto binding_it = std::ranges::find_if(
                        *source_bindings, [](const ViewLeafBinding& binding) {
                            return binding.path.empty();
                        });
                    if (binding_it != source_bindings->end() &&
                        !binding_it->element_sources.empty()) {
                        expr.cached_view_bindings =
                            std::vector<ast::CachedViewBinding>{
                                ast::CachedViewBinding{
                                    .path = {},
                                    .source_place = binding_it->source_place,
                                    .source_local_id =
                                        binding_it->source_local_id,
                                    .element_sources =
                                        binding_it->element_sources,
                                    .type = expr.resolved_type,
                                },
                            };
                    }
                }
                return expr.resolved_type;
            },
            [&](ast::InitListExpr& init_list)
                -> std::expected<const Type*, Diagnostic> {
                if (expected_type == nullptr ||
                    expected_type->kind != TypeKind::Struct ||
                    expected_type->struct_decl == nullptr) {
                    return unexpected_result<const Type*>(
                        "initializer list requires an expected struct type",
                        expr.range);
                }

                const auto& fields = expected_type->struct_decl->fields;
                if (init_list.elements.size() != fields.size()) {
                    return unexpected_result<const Type*>(
                        "initializer list element count does not match struct "
                        "field count",
                        expr.range);
                }

                std::vector<ast::CachedViewBinding> cached_bindings;
                for (std::size_t index = 0; index < init_list.elements.size();
                     ++index) {
                    auto& element_expr = *init_list.elements[index];
                    const auto borrow_field_already_checked =
                        fields[index].resolved_type->kind == TypeKind::Borrow &&
                        element_expr.resolved_type != nullptr &&
                        types.isSame(element_expr.resolved_type,
                                     fields[index].resolved_type);
                    if (fields[index].resolved_type->kind == TypeKind::Borrow &&
                        !borrow_field_already_checked) {
                        auto borrowed = borrowFromExpr(
                            state, element_expr,
                            fields[index].resolved_type->is_mut, true);
                        if (!borrowed) {
                            return std::unexpected(borrowed.error());
                        }
                    }

                    if (typeContainsViews(fields[index].resolved_type)) {
                        auto analyzed = analyzeExpr(
                            state, element_expr, fields[index].resolved_type);
                        if (!analyzed) {
                            return std::unexpected(analyzed.error());
                        }
                        auto bindings =
                            collectExprViewBindings(state, element_expr);
                        if (!bindings) {
                            return std::unexpected(bindings.error());
                        }
                        for (const auto& binding : *bindings) {
                            auto path = binding.path;
                            path.insert(path.begin(),
                                        static_cast<std::uint32_t>(index));
                            cached_bindings.push_back(ast::CachedViewBinding{
                                .path = std::move(path),
                                .source_place = binding.source_place,
                                .source_local_id = binding.source_local_id,
                                .element_sources = binding.element_sources,
                                .type = binding.type,
                            });
                        }
                    }

                    if (fields[index].resolved_type->kind == TypeKind::Borrow) {
                        if (!types.isSame(element_expr.resolved_type,
                                          fields[index].resolved_type)) {
                            return unexpected_result<const Type*>(
                                "initializer field type does not match",
                                element_expr.range);
                        }
                    } else {
                        auto element = consumeValue(
                            state, element_expr, fields[index].resolved_type);
                        if (!element) {
                            return std::unexpected(element.error());
                        }
                    }
                }

                expr.resolved_type = expected_type;
                expr.resolved_place.reset();
                if (!cached_bindings.empty()) {
                    expr.cached_view_bindings = std::move(cached_bindings);
                }
                return expr.resolved_type;
            },
            [&](ast::ArrayLiteralExpr& array_literal)
                -> std::expected<const Type*, Diagnostic> {
                if (expected_type == nullptr ||
                    (types.unqualify(expected_type)->kind != TypeKind::Array &&
                     types.unqualify(expected_type)->kind != TypeKind::Slice)) {
                    return unexpected_result<const Type*>(
                        "array literal requires an expected array or slice "
                        "type",
                        expr.range);
                }
                const auto* expected_base = types.unqualify(expected_type);
                if (expected_base->kind == TypeKind::Array &&
                    array_literal.elements.size() !=
                        expected_base->array_size) {
                    return unexpected_result<const Type*>(
                        "array literal element count does not match array size",
                        expr.range);
                }

                std::optional<ast::ResolvedPlace> most_restrictive_source;
                const auto* elem_type =
                    types.unqualify(expected_base->element_type);
                const bool elem_is_borrow_like = is_borrow_like_type(elem_type);
                const bool track_element_sources =
                    expected_base->kind == TypeKind::Slice &&
                    is_direct_shared_view_slice(types, expected_type);
                std::vector<ast::ResolvedPlace> element_sources;

                for (auto& element : array_literal.elements) {
                    auto analyzed = consumeValue(state, *element,
                                                 expected_base->element_type);
                    if (!analyzed) {
                        return std::unexpected(analyzed.error());
                    }
                    if ((elem_is_borrow_like || track_element_sources) &&
                        expected_base->kind == TypeKind::Slice) {
                        auto src = borrowFromExpr(state, *element,
                                                  elem_type->is_mut, false);
                        if (!src) {
                            return std::unexpected(src.error());
                        }
                        if (track_element_sources &&
                            std::ranges::find(element_sources, *src) ==
                                element_sources.end()) {
                            element_sources.push_back(*src);
                        }
                        if (!most_restrictive_source.has_value()) {
                            most_restrictive_source = *src;
                        } else if (!src->is_external) {
                            const auto src_idx =
                                findLocalById(state, src->root_id);
                            const auto cur_idx = findLocalById(
                                state, most_restrictive_source->root_id);
                            if (src_idx.has_value() &&
                                (!cur_idx.has_value() ||
                                 state.locals[*src_idx].scope_depth >
                                     state.locals[*cur_idx].scope_depth)) {
                                most_restrictive_source = *src;
                            }
                        }
                    }
                }
                if (expected_base->kind == TypeKind::Slice &&
                    !types.isCopy(expected_base->element_type)) {
                    if (expr.slice_storage_local_id == 0) {
                        const auto* storage_type =
                            types.getArray(expected_base->element_type,
                                           array_literal.elements.size());
                        expr.slice_storage_local_id =
                            state
                                .locals[declareHiddenLocal(state, storage_type)]
                                .unique_id;
                    }
                    expr.slice_source_place =
                        localPlace(expr.slice_storage_local_id);
                } else if (most_restrictive_source.has_value()) {
                    // Slice of copy-borrow-like elements: no backing storage
                    // local, but we still need to track the most restrictive
                    // element source so lifetime checks work.
                    expr.slice_source_place = most_restrictive_source;
                } else {
                    expr.slice_source_place.reset();
                }
                expr.resolved_type = expected_type;
                expr.resolved_place.reset();
                if (track_element_sources) {
                    expr.cached_view_bindings =
                        std::vector<ast::CachedViewBinding>{
                            ast::CachedViewBinding{
                                .path = {},
                                .source_place = expr.slice_source_place,
                                .source_local_id = std::nullopt,
                                .element_sources = std::move(element_sources),
                                .type = expected_type,
                            },
                        };
                }
                return expr.resolved_type;
            },
            [&](ast::CastExpr& cast_expr)
                -> std::expected<const Type*, Diagnostic> {
                auto target_type = resolveType(*cast_expr.target_type);
                if (!target_type) {
                    return std::unexpected(target_type.error());
                }
                auto operand_type = analyzeExpr(state, *cast_expr.operand);
                if (!operand_type) {
                    return std::unexpected(operand_type.error());
                }
                if (types.sameIgnoringConst(*operand_type, *target_type) &&
                    !types.isSame(*operand_type, *target_type)) {
                    if (state.unchecked_depth == 0) {
                        return unexpected_result<const Type*>(
                            "const cast is only allowed in unchecked blocks",
                            expr.range);
                    }
                    cast_expr.cast_kind = ast::CastKind::Const;
                    expr.resolved_type = *target_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                if (*operand_type == *target_type) {
                    cast_expr.cast_kind = ast::CastKind::Numeric;
                    expr.resolved_type = *target_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                const auto source_numeric =
                    types.unqualify(*operand_type) == types.intType() ||
                    types.unqualify(*operand_type) == types.floatType() ||
                    types.unqualify(*operand_type) == types.charType();
                const auto target_numeric =
                    types.unqualify(*target_type) == types.intType() ||
                    types.unqualify(*target_type) == types.floatType() ||
                    types.unqualify(*target_type) == types.charType();
                if (source_numeric && target_numeric) {
                    cast_expr.cast_kind = ast::CastKind::Numeric;
                    expr.resolved_type = *target_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                if (state.unchecked_depth > 0 &&
                    types.unqualify(*operand_type)->kind == TypeKind::Pointer &&
                    types.unqualify(*target_type)->kind == TypeKind::Pointer) {
                    cast_expr.cast_kind = ast::CastKind::Pointer;
                    expr.resolved_type = *target_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                return unexpected_result<const Type*>("invalid cast",
                                                      expr.range);
            },
            [&](ast::SizeofExpr& sizeof_expr)
                -> std::expected<const Type*, Diagnostic> {
                auto operand_type = resolveType(*sizeof_expr.type);
                if (!operand_type) {
                    return std::unexpected(operand_type.error());
                }
                sizeof_expr.operand_type = *operand_type;
                expr.resolved_type = types.intType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
        },
        expr.node);
}

auto SemanticAnalyzer::analyzeCall(FunctionState& state, ast::Expr& expr,
                                   ast::CallExpr& call,
                                   const Type* expected_type)
    -> std::expected<const Type*, Diagnostic> {
    auto find_variant_index =
        [](const ast::EnumDecl& enum_decl,
           std::string_view variant_name) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < enum_decl.variants.size();
             ++index) {
            if (enum_decl.variants[index].name == variant_name) {
                return index;
            }
        }
        return std::nullopt;
    };

    auto analyze_argument =
        [&](ast::Expr& argument,
            const Type* parameter_type) -> std::expected<void, Diagnostic> {
        if (parameter_type->kind == TypeKind::Borrow) {
            const auto* name = std::get_if<ast::NameExpr>(&argument.node);
            auto arg_type = analyzeExpr(state, argument, parameter_type);
            if (!arg_type) {
                return std::unexpected(arg_type.error());
            }
            auto compatible =
                ensureBorrowSourceType(state, argument, parameter_type);
            if (!compatible) {
                return std::unexpected(compatible.error());
            }

            if (*arg_type == parameter_type && name != nullptr) {
                const auto local_index = findLocalById(state, name->local_id);
                if (!local_index.has_value()) {
                    return unexpected_result<void>("invalid borrow argument",
                                                   argument.range);
                }
                auto& local = state.locals[*local_index];
                if (parameter_type->is_mut) {
                    if (local.type->kind != TypeKind::Borrow ||
                        !local.type->is_mut ||
                        !local.borrow_origin.has_value()) {
                        return unexpected_result<void>(
                            "expected a mutable borrow argument",
                            argument.range);
                    }
                    auto borrow =
                        ensureCanBorrow(state, *local.borrow_origin, true,
                                        argument.range, local.unique_id);
                    if (!borrow) {
                        return std::unexpected(borrow.error());
                    }
                    auto loan_place = *local.borrow_origin;
                    loan_place.owner_local_id = local.unique_id;
                    local.status = LocalState::Status::Moved;
                    state.temporary_loans.push_back(
                        TemporaryLoan{loan_place, true});
                    state.temporary_suspended_local_ids.push_back(
                        local.unique_id);
                } else if (local.type->kind == TypeKind::Borrow &&
                           local.type->is_mut &&
                           local.borrow_origin.has_value()) {
                    auto borrow =
                        ensureCanBorrow(state, *local.borrow_origin, false,
                                        argument.range, local.unique_id);
                    if (!borrow) {
                        return std::unexpected(borrow.error());
                    }
                    auto loan_place = *local.borrow_origin;
                    state.temporary_loans.push_back(
                        TemporaryLoan{loan_place, false});
                } else if (local.type->kind != TypeKind::Borrow ||
                           local.borrow_origin == std::nullopt) {
                    return unexpected_result<void>("expected a borrow argument",
                                                   argument.range);
                }
                return {};
            }

            auto borrowed =
                borrowFromExpr(state, argument, parameter_type->is_mut, true);
            if (!borrowed) {
                return std::unexpected(borrowed.error());
            }
            return {};
        }

        if (parameter_type->kind == TypeKind::Interface) {
            const auto* name = std::get_if<ast::NameExpr>(&argument.node);
            auto arg_type = analyzeExpr(state, argument, parameter_type);
            if (!arg_type) {
                return std::unexpected(arg_type.error());
            }

            if (*arg_type == parameter_type) {
                if (name != nullptr) {
                    const auto local_index =
                        findLocalById(state, name->local_id);
                    if (local_index.has_value()) {
                        auto& local = state.locals[*local_index];
                        if (local.type->kind == TypeKind::Interface &&
                            local.borrow_origin.has_value()) {
                            if (parameter_type->is_mut) {
                                auto borrow = ensureCanBorrow(
                                    state, *local.borrow_origin, true,
                                    argument.range, local.unique_id);
                                if (!borrow) {
                                    return std::unexpected(borrow.error());
                                }
                                auto loan_place = *local.borrow_origin;
                                loan_place.owner_local_id = local.unique_id;
                                local.status = LocalState::Status::Moved;
                                state.temporary_loans.push_back(
                                    TemporaryLoan{loan_place, true});
                                state.temporary_suspended_local_ids.push_back(
                                    local.unique_id);
                            }
                            return {};
                        }
                    }
                }

                auto borrowed = borrowFromExpr(state, argument,
                                               parameter_type->is_mut, true);
                if (!borrowed) {
                    return std::unexpected(borrowed.error());
                }
                return {};
            }

            auto borrowed =
                borrowFromExpr(state, argument, parameter_type->is_mut, true);
            if (!borrowed) {
                return std::unexpected(borrowed.error());
            }
            auto coerced = analyzeExpr(state, argument, parameter_type);
            if (!coerced) {
                return std::unexpected(coerced.error());
            }
            return {};
        }

        std::vector<ViewLeafBinding> call_view_bindings;
        if (typeContainsViews(parameter_type)) {
            auto analyzed = analyzeExpr(state, argument, parameter_type);
            if (!analyzed) {
                return std::unexpected(analyzed.error());
            }
            auto bindings = collectExprViewBindings(state, argument);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            call_view_bindings = std::move(*bindings);
        }

        auto value = consumeValue(state, argument, parameter_type);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (state.unchecked_depth > 0 &&
            parameter_type->kind == TypeKind::Pointer &&
            parameter_type->element_type != nullptr &&
            !parameter_type->element_type->is_const) {
            if (const auto* unary = std::get_if<ast::UnaryExpr>(&argument.node);
                unary != nullptr && unary->op == ast::UnaryOp::BorrowMut) {
                state.relaxed_place_resolution_depth++;
                auto place = resolvePlace(state, *unary->operand);
                state.relaxed_place_resolution_depth--;
                if (!place) {
                    return std::unexpected(place.error());
                }
                if (!place->is_external) {
                    const auto local_index =
                        findLocalById(state, place->root_id);
                    if (local_index.has_value()) {
                        state.locals[*local_index].status =
                            LocalState::Status::Live;
                    }
                }
            }
        }
        for (const auto& binding : call_view_bindings) {
            if (!binding.element_sources.empty()) {
                for (auto loan_place : binding.element_sources) {
                    state.temporary_loans.push_back(TemporaryLoan{
                        .place = std::move(loan_place),
                        .is_mut = false,
                    });
                }
                continue;
            }
            if (!binding.source_place.has_value()) {
                continue;
            }
            auto loan_place = *binding.source_place;
            const auto binding_is_mut =
                is_borrow_like_type(binding.type) && binding.type->is_mut;
            if (binding_is_mut && binding.source_local_id.has_value()) {
                loan_place.owner_local_id = *binding.source_local_id;
            }
            state.temporary_loans.push_back(TemporaryLoan{
                .place = std::move(loan_place),
                .is_mut = binding_is_mut,
            });
        }
        return {};
    };

    ast::FunctionDecl* function = nullptr;
    const auto& scope = visibleScopeFor(*active_module);
    const auto builtin_available =
        !scope.functions.contains(call.callee) &&
        !scope.function_templates.contains(call.callee) &&
        findVisibleInterface(call.callee) == nullptr &&
        !scope.variants.contains(call.callee) &&
        !scope.template_variants.contains(call.callee);
    if (call.callee == "len" && builtin_available) {
        if (call.arguments.size() != 1) {
            return unexpected_result<const Type*>(
                "len() expects exactly one argument", expr.range);
        }

        auto argument_type = analyzeExpr(state, *call.arguments.front());
        if (!argument_type) {
            return std::unexpected(argument_type.error());
        }
        const auto* argument_base = types.unqualify(*argument_type);
        if (argument_base->kind != TypeKind::Array &&
            argument_base->kind != TypeKind::Slice) {
            return unexpected_result<const Type*>(
                "len() requires an array or slice argument", expr.range);
        }

        call.builtin_kind = ast::BuiltinCallKind::Len;
        expr.resolved_type = types.intType();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (call.callee == "subslice" && builtin_available) {
        if (call.arguments.size() != 3) {
            return unexpected_result<const Type*>(
                "subslice() expects exactly three arguments", expr.range);
        }

        auto source_type = analyzeExpr(state, *call.arguments[0]);
        if (!source_type) {
            return std::unexpected(source_type.error());
        }

        const auto* source_base = types.unqualify(*source_type);
        const Type* element_type = nullptr;
        if (source_base->kind == TypeKind::Array) {
            element_type = source_base->element_type;
            if ((*source_type)->is_const && !element_type->is_const) {
                element_type = types.getConst(element_type);
            }
            if (call.arguments[0]->resolved_place.has_value()) {
                expr.slice_source_place = *call.arguments[0]->resolved_place;
            }
        } else if (source_base->kind == TypeKind::Borrow &&
                   source_base->element_type != nullptr &&
                   types.unqualify(source_base->element_type)->kind ==
                       TypeKind::Array) {
            const auto* array_type_raw = source_base->element_type;
            const auto* array_type = types.unqualify(array_type_raw);
            element_type = array_type->element_type;
            if (array_type_raw->is_const && !element_type->is_const) {
                element_type = types.getConst(element_type);
            }
            auto source_place = borrowSourcePlace(state, *call.arguments[0]);
            if (!source_place) {
                return std::unexpected(source_place.error());
            }
            expr.slice_source_place = *source_place;
        } else if (source_base->kind == TypeKind::Slice) {
            element_type = source_base->element_type;
            auto source_place = sliceSourcePlace(state, *call.arguments[0]);
            if (!source_place) {
                return std::unexpected(source_place.error());
            }
            expr.slice_source_place = *source_place;
        } else {
            return unexpected_result<const Type*>(
                "subslice() requires an array, array borrow, or slice source",
                call.arguments[0]->range);
        }

        for (std::size_t index = 1; index < call.arguments.size(); ++index) {
            auto argument_type =
                analyzeExpr(state, *call.arguments[index], types.intType());
            if (!argument_type) {
                return std::unexpected(argument_type.error());
            }
            if (types.unqualify(*argument_type) != types.intType()) {
                return unexpected_result<const Type*>(
                    "subslice() offset and length must have type int",
                    call.arguments[index]->range);
            }
        }

        const auto* result_type = types.getSlice(element_type);
        if (expected_type != nullptr &&
            types.unqualify(expected_type)->kind == TypeKind::Slice &&
            can_consume_value_type(types, result_type, expected_type)) {
            result_type = expected_type;
        }

        call.builtin_kind = ast::BuiltinCallKind::Subslice;
        expr.resolved_type = result_type;
        expr.resolved_place.reset();
        if (is_direct_shared_view_slice(types, result_type)) {
            auto source_bindings =
                collectExprViewBindings(state, *call.arguments[0]);
            if (!source_bindings) {
                return std::unexpected(source_bindings.error());
            }
            const auto binding_it = std::ranges::find_if(
                *source_bindings, [](const ViewLeafBinding& binding) {
                    return binding.path.empty();
                });
            if (binding_it != source_bindings->end() &&
                !binding_it->element_sources.empty()) {
                expr.cached_view_bindings = std::vector<ast::CachedViewBinding>{
                    ast::CachedViewBinding{
                        .path = {},
                        .source_place = expr.slice_source_place,
                        .source_local_id = binding_it->source_local_id,
                        .element_sources = binding_it->element_sources,
                        .type = result_type,
                    },
                };
            }
        }
        return expr.resolved_type;
    }
    if (const auto function_it = scope.functions.find(call.callee);
        function_it != scope.functions.end()) {
        function = function_it->second;
    } else {
        auto template_it = scope.function_templates.find(call.callee);
        if (template_it != scope.function_templates.end()) {
            std::vector<ast::Expr*> arguments;
            arguments.reserve(call.arguments.size());
            for (const auto& argument : call.arguments) {
                arguments.push_back(argument.get());
            }
            auto type_bindings =
                inferTypeBindings(state, *template_it->second, arguments);
            if (!type_bindings) {
                return std::unexpected(type_bindings.error());
            }
            auto instantiated = instantiateFunctionTemplate(
                *template_it->second, *type_bindings);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            function = *instantiated;
        }
    }

    const auto* interface_decl = findVisibleInterface(call.callee);
    if (function == nullptr && interface_decl != nullptr) {
        if (call.arguments.size() != interface_decl->parameters.size()) {
            return unexpected_result<const Type*>(
                "call argument count does not match interface signature",
                expr.range);
        }

        auto& receiver_argument = *call.arguments.front();
        auto receiver_type = analyzeExpr(state, receiver_argument);
        if (!receiver_type) {
            return std::unexpected(receiver_type.error());
        }

        const auto* interface_type = types.getInterface(interface_decl);
        if (types.sameIgnoringTopLevelConst(*receiver_type, interface_type)) {
            call.dispatched_interface = interface_decl;
            auto analyzed_receiver =
                analyze_argument(receiver_argument, interface_type);
            if (!analyzed_receiver) {
                return std::unexpected(analyzed_receiver.error());
            }
            for (std::size_t index = 1; index < call.arguments.size();
                 ++index) {
                auto analyzed_argument = analyze_argument(
                    *call.arguments[index],
                    interface_decl->parameters[index].resolved_type);
                if (!analyzed_argument) {
                    return std::unexpected(analyzed_argument.error());
                }
            }
            expr.resolved_type = interface_decl->resolved_return_type;
            expr.resolved_place.reset();
            return expr.resolved_type;
        }

        const auto* concrete_receiver = interfaceReceiverType(*receiver_type);
        if (concrete_receiver == nullptr) {
            return unexpected_result<const Type*>(
                "interface call requires a concrete receiver or interface "
                "value",
                receiver_argument.range);
        }

        if ((*receiver_type)->kind == TypeKind::Struct ||
            (*receiver_type)->kind == TypeKind::Enum) {
            auto borrowed_receiver = std::make_unique<ast::Expr>();
            borrowed_receiver->range = receiver_argument.range;
            borrowed_receiver->node = ast::UnaryExpr{
                .op = interface_decl->receiver_is_mut ? ast::UnaryOp::BorrowMut
                                                      : ast::UnaryOp::Borrow,
                .operand = std::move(call.arguments.front())};
            call.arguments.front() = std::move(borrowed_receiver);
        } else if ((*receiver_type)->kind == TypeKind::Borrow &&
                   interface_decl->receiver_is_mut &&
                   !(*receiver_type)->is_mut) {
            return unexpected_result<const Type*>(
                "interface requires a mutable receiver borrow",
                receiver_argument.range);
        }

        auto impl = findImplForType(*interface_decl, concrete_receiver);
        if (!impl) {
            return std::unexpected(impl.error());
        }
        function = *impl;
    }

    if (function == nullptr) {
        ast::EnumDecl* enum_decl = nullptr;
        std::size_t variant_index = 0;

        if (expected_type != nullptr && expected_type->kind == TypeKind::Enum &&
            expected_type->enum_decl != nullptr) {
            const auto expected_variant =
                find_variant_index(*expected_type->enum_decl, call.callee);
            if (!expected_variant.has_value()) {
                return unexpected_result<const Type*>(
                    "unknown enum variant '" + call.callee + "'", expr.range);
            }
            enum_decl = const_cast<ast::EnumDecl*>(expected_type->enum_decl);
            variant_index = *expected_variant;
        } else if (const auto variant_it = scope.variants.find(call.callee);
                   variant_it != scope.variants.end()) {
            enum_decl = variant_it->second.first;
            variant_index = variant_it->second.second;
        } else if (const auto template_variant_it =
                       scope.template_variants.find(call.callee);
                   template_variant_it != scope.template_variants.end()) {
            ast::EnumDecl* selected_template = nullptr;
            std::size_t selected_variant_index = 0;
            TypeBindings selected_bindings;

            for (const auto& [candidate_decl, candidate_variant_index] :
                 template_variant_it->second) {
                const auto& candidate_variant =
                    candidate_decl->variants[candidate_variant_index];
                const auto expected_arguments =
                    candidate_variant.payload_type == nullptr ? std::size_t{0}
                                                              : std::size_t{1};
                if (call.arguments.size() != expected_arguments) {
                    continue;
                }

                TypeBindings candidate_bindings;
                if (candidate_variant.payload_type != nullptr) {
                    auto argument_type =
                        analyzeExpr(state, *call.arguments.front());
                    if (!argument_type) {
                        return std::unexpected(argument_type.error());
                    }
                    auto matched =
                        matchTypePattern(*candidate_variant.payload_type,
                                         *candidate_decl->owner_module,
                                         candidate_decl->type_parameters,
                                         *argument_type, candidate_bindings);
                    if (!matched) {
                        continue;
                    }
                }

                bool complete = true;
                for (const auto& type_parameter :
                     candidate_decl->type_parameters) {
                    if (!candidate_bindings.contains(type_parameter)) {
                        complete = false;
                        break;
                    }
                }
                if (!complete) {
                    continue;
                }

                if (selected_template != nullptr) {
                    return unexpected_result<const Type*>(
                        "generic enum constructor is ambiguous", expr.range);
                }

                selected_template = candidate_decl;
                selected_variant_index = candidate_variant_index;
                selected_bindings = std::move(candidate_bindings);
            }

            if (selected_template == nullptr) {
                return unexpected_result<const Type*>(
                    "could not infer generic enum constructor '" + call.callee +
                        "'",
                    expr.range);
            }

            std::vector<const Type*> type_arguments;
            type_arguments.reserve(selected_template->type_parameters.size());
            for (const auto& type_parameter :
                 selected_template->type_parameters) {
                type_arguments.push_back(selected_bindings.at(type_parameter));
            }
            auto instantiated =
                instantiateEnumTemplate(*selected_template, type_arguments);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            enum_decl = *instantiated;
            variant_index = selected_variant_index;
        } else {
            return unexpected_result<const Type*>(
                "unknown function or enum variant '" + call.callee + "'",
                expr.range);
        }

        auto& variant = enum_decl->variants[variant_index];
        call.enum_decl = enum_decl;
        call.variant_index = static_cast<std::uint32_t>(variant_index);
        const auto expected_arguments =
            variant.resolved_type == nullptr ? std::size_t{0} : std::size_t{1};
        if (call.arguments.size() != expected_arguments) {
            return unexpected_result<const Type*>(
                "enum constructor argument count does not match variant "
                "payload",
                expr.range);
        }
        if (variant.resolved_type != nullptr) {
            if (typeContainsViews(variant.resolved_type)) {
                auto analyzed_payload = analyzeExpr(
                    state, *call.arguments.front(), variant.resolved_type);
                if (!analyzed_payload) {
                    return std::unexpected(analyzed_payload.error());
                }
                auto bindings =
                    collectExprViewBindings(state, *call.arguments.front());
                if (!bindings) {
                    return std::unexpected(bindings.error());
                }
                std::vector<ast::CachedViewBinding> cached_bindings;
                cached_bindings.reserve(bindings->size());
                for (const auto& binding : *bindings) {
                    auto path = binding.path;
                    path.insert(path.begin(), call.variant_index);
                    path.insert(path.begin(), ENUM_PAYLOAD_SENTINEL);
                    cached_bindings.push_back(ast::CachedViewBinding{
                        .path = std::move(path),
                        .source_place = binding.source_place,
                        .source_local_id = binding.source_local_id,
                        .element_sources = binding.element_sources,
                        .type = binding.type,
                    });
                }
                expr.cached_view_bindings = std::move(cached_bindings);
            }
            if (variant.resolved_type->kind == TypeKind::Borrow) {
                auto& payload_expr = *call.arguments.front();
                const auto borrow_payload_already_checked =
                    payload_expr.resolved_type != nullptr &&
                    types.isSame(payload_expr.resolved_type,
                                 variant.resolved_type);
                if (!borrow_payload_already_checked) {
                    auto borrowed =
                        borrowFromExpr(state, payload_expr,
                                       variant.resolved_type->is_mut, true);
                    if (!borrowed) {
                        return std::unexpected(borrowed.error());
                    }
                }
                auto analyzed =
                    analyzeExpr(state, payload_expr, variant.resolved_type);
                if (!analyzed) {
                    return std::unexpected(analyzed.error());
                }
                if (!types.isSame(*analyzed, variant.resolved_type)) {
                    return unexpected_result<const Type*>(
                        "enum constructor payload type does not match",
                        payload_expr.range);
                }
            } else {
                auto value = consumeValue(state, *call.arguments.front(),
                                          variant.resolved_type);
                if (!value) {
                    return std::unexpected(value.error());
                }
            }
        }

        expr.resolved_type = enum_decl->resolved_type;
        expr.resolved_place.reset();
        return expr.resolved_type;
    }

    auto& resolved_function = *function;
    call.function = &resolved_function;
    call.callee = resolved_function.name;
    if (resolved_function.resolved_return_type == nullptr ||
        std::ranges::any_of(resolved_function.parameters,
                            [](const ast::Parameter& parameter) {
                                return parameter.resolved_type == nullptr;
                            })) {
        return unexpected_result<const Type*>(
            "cannot call a function with an invalid type signature",
            expr.range);
    }
    if (call.arguments.size() != resolved_function.parameters.size()) {
        return unexpected_result<const Type*>(
            "call argument count does not match function signature",
            expr.range);
    }

    std::unordered_map<std::size_t, std::vector<ViewLeafBinding>>
        dependency_argument_bindings;
    if (typeContainsViews(resolved_function.resolved_return_type)) {
        for (const auto& dependency : resolved_function.return_dependencies) {
            if (!dependency.source.parameter_index.has_value()) {
                continue;
            }
            const auto parameter_index = *dependency.source.parameter_index;
            if (dependency_argument_bindings.contains(parameter_index)) {
                continue;
            }
            auto analyzed_argument = analyzeExpr(
                state, *call.arguments[parameter_index],
                resolved_function.parameters[parameter_index].resolved_type);
            if (!analyzed_argument) {
                return std::unexpected(analyzed_argument.error());
            }
            auto bindings = collectExprViewBindings(
                state, *call.arguments[parameter_index]);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            dependency_argument_bindings.emplace(parameter_index,
                                                 std::move(*bindings));
        }
    }

    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        auto analyzed_argument =
            analyze_argument(*call.arguments[index],
                             resolved_function.parameters[index].resolved_type);
        if (!analyzed_argument) {
            return std::unexpected(analyzed_argument.error());
        }
    }

    expr.resolved_type = resolved_function.resolved_return_type;
    if (typeContainsViews(expr.resolved_type) &&
        !resolved_function.return_dependencies.empty()) {
        std::vector<ast::CachedViewBinding> cached_bindings;
        auto find_binding =
            [](std::vector<ViewLeafBinding>& bindings,
               const std::vector<std::uint32_t>& path) -> ViewLeafBinding* {
            const auto it = std::ranges::find_if(
                bindings, [&](const ViewLeafBinding& binding) {
                    return binding.path == path;
                });
            return it == bindings.end() ? nullptr : &(*it);
        };

        for (const auto& dependency : resolved_function.return_dependencies) {
            if (!dependency.source.parameter_index.has_value()) {
                continue;
            }
            const auto parameter_index = *dependency.source.parameter_index;
            auto& source_bindings =
                dependency_argument_bindings.at(parameter_index);
            const auto target_leaves =
                collectViewLeafInfos(dependency.target.resolved_type);
            const auto source_leaves =
                collectViewLeafInfos(dependency.source.resolved_type);
            for (std::size_t index = 0; index < target_leaves.size(); ++index) {
                auto source_path = dependency.source.resolved_path;
                source_path.insert(source_path.end(),
                                   source_leaves[index].path.begin(),
                                   source_leaves[index].path.end());
                auto* source_binding =
                    find_binding(source_bindings, source_path);
                if (source_binding == nullptr) {
                    return unexpected_result<const Type*>(
                        "call dependency source is missing a tracked borrow or "
                        "slice leaf",
                        expr.range);
                }

                auto target_path = dependency.target.resolved_path;
                target_path.insert(target_path.end(),
                                   target_leaves[index].path.begin(),
                                   target_leaves[index].path.end());
                cached_bindings.push_back(ast::CachedViewBinding{
                    .path = std::move(target_path),
                    .source_place = source_binding->source_place,
                    .source_local_id = source_binding->source_local_id,
                    .element_sources = source_binding->element_sources,
                    .type = target_leaves[index].type,
                });
            }
        }

        expr.cached_view_bindings = cached_bindings;
        if (is_view_like_type(types, expr.resolved_type)) {
            const auto it = std::ranges::find_if(
                cached_bindings, [](const ast::CachedViewBinding& binding) {
                    return binding.path.empty();
                });
            if (it == cached_bindings.end()) {
                return unexpected_result<const Type*>(
                    "call return is missing top-level borrow or slice "
                    "provenance",
                    expr.range);
            }
            if (is_borrow_like_type(expr.resolved_type)) {
                if (!it->source_place.has_value()) {
                    return unexpected_result<const Type*>(
                        "borrow-returning call has no tracked source",
                        expr.range);
                }
                expr.resolved_place = *it->source_place;
                if (expr.resolved_type->is_mut &&
                    it->source_local_id.has_value()) {
                    expr.resolved_place->owner_local_id = *it->source_local_id;
                }
                expr.slice_source_place.reset();
            } else {
                expr.slice_source_place = it->source_place;
                expr.resolved_place.reset();
            }
        } else {
            expr.resolved_place.reset();
            expr.slice_source_place.reset();
        }
    } else if (types.unqualify(expr.resolved_type)->kind == TypeKind::Slice) {
        expr.cached_view_bindings.reset();
        expr.resolved_place.reset();
    } else {
        expr.cached_view_bindings.reset();
        expr.resolved_place.reset();
        expr.slice_source_place.reset();
    }
    return expr.resolved_type;
}

auto SemanticAnalyzer::analyzeMember(FunctionState& state, ast::Expr& expr,
                                     ast::MemberExpr& member)
    -> std::expected<const Type*, Diagnostic> {
    auto base_type = analyzeExpr(state, *member.base);
    if (!base_type) {
        return std::unexpected(base_type.error());
    }

    ast::ResolvedPlace base_place;
    const Type* struct_base_type = *base_type;
    if ((*base_type)->kind == TypeKind::Struct) {
        auto resolved_place = resolvePlace(state, *member.base);
        if (!resolved_place) {
            return std::unexpected(resolved_place.error());
        }
        base_place = *resolved_place;
    } else if ((*base_type)->kind == TypeKind::Borrow &&
               (*base_type)->element_type != nullptr &&
               (*base_type)->element_type->kind == TypeKind::Struct) {
        auto source_place = borrowSourcePlace(state, *member.base);
        if (!source_place) {
            return std::unexpected(source_place.error());
        }
        base_place = *source_place;
        if ((*base_type)->is_mut) {
            auto source_local_id = borrowSourceLocalId(state, *member.base);
            if (!source_local_id) {
                return std::unexpected(source_local_id.error());
            }
            if (source_local_id->has_value()) {
                base_place.owner_local_id = **source_local_id;
            }
        }
        struct_base_type = (*base_type)->element_type;
    } else {
        return unexpected_result<const Type*>(
            "field access requires a struct value", member.base->range);
    }

    const auto& fields = struct_base_type->struct_decl->fields;
    const auto field_it =
        std::ranges::find_if(fields, [&](const ast::StructField& field) {
            return field.name == member.field_name;
        });
    if (field_it == fields.end()) {
        return unexpected_result<const Type*>(
            "unknown field '" + member.field_name + "'", expr.range);
    }

    member.field_index =
        static_cast<std::uint32_t>(std::distance(fields.begin(), field_it));
    expr.resolved_type = field_it->resolved_type;
    if (struct_base_type->is_const && !expr.resolved_type->is_const) {
        expr.resolved_type = types.getConst(expr.resolved_type);
    }
    expr.resolved_place = base_place;
    expr.resolved_place->fields.push_back(member.field_index);
    if (typeContainsViews(expr.resolved_type)) {
        auto live = ensureViewSubtreeLive(state, *expr.resolved_place,
                                          expr.resolved_type, expr.range);
        if (!live) {
            return std::unexpected(live.error());
        }
    }
    return expr.resolved_type;
}

} // namespace sc

#include "sema_detail.hpp"

namespace sc {

using namespace detail;

namespace {

auto view_source_kind(const TypeContext& types, const Type* type)
    -> std::string_view {
    if (type == nullptr) {
        return "borrow";
    }
    const auto* base = types.unqualify(type);
    return base->kind == TypeKind::Slice ? "slice" : "borrow";
}

auto has_direct_interface_source(const ast::Expr& expr) -> bool {
    return expr.resolved_type != nullptr &&
           expr.resolved_type->kind == TypeKind::Interface &&
           expr.interface_source_type != nullptr &&
           expr.interface_source_type->kind != TypeKind::Borrow &&
           expr.resolved_place.has_value();
}

auto view_source_places(const auto& local) -> std::vector<ast::ResolvedPlace> {
    if (!local.element_origins.empty()) {
        return local.element_origins;
    }
    if (local.borrow_origin.has_value()) {
        return {*local.borrow_origin};
    }
    return {};
}

} // namespace

auto SemanticAnalyzer::borrowSourcePlace(FunctionState& state, ast::Expr& expr)
    -> std::expected<ast::ResolvedPlace, Diagnostic> {
    if (expr.cached_view_bindings.has_value()) {
        const auto it =
            std::ranges::find_if(*expr.cached_view_bindings,
                                 [](const ast::CachedViewBinding& binding) {
                                     return binding.path.empty();
                                 });
        if (it != expr.cached_view_bindings->end() &&
            it->source_place.has_value()) {
            return *it->source_place;
        }
    }

    if (const auto* name = std::get_if<ast::NameExpr>(&expr.node);
        name != nullptr) {
        auto local_id = name->local_id;
        if (local_id == 0) {
            const auto local_index = lookupLocal(state, name->name);
            if (local_index.has_value()) {
                local_id = state.locals[*local_index].unique_id;
            }
        }
        if (local_id != 0) {
            const auto local_index = findLocalById(state, local_id);
            if (local_index.has_value()) {
                const auto& local = state.locals[*local_index];
                if (is_borrow_like_type(local.type) &&
                    local.borrow_origin.has_value()) {
                    return *local.borrow_origin;
                }
            }
        }
    }

    if (expr.resolved_place.has_value() && expr.resolved_type != nullptr &&
        is_borrow_like_type(expr.resolved_type)) {
        if (has_direct_interface_source(expr)) {
            return *expr.resolved_place;
        }
        if (const auto slot_index = findViewSlotLocal(
                state, expr.resolved_place->is_external,
                expr.resolved_place->root_id, expr.resolved_place->fields);
            slot_index.has_value()) {
            const auto& slot = state.locals[*slot_index];
            if (slot.borrow_origin.has_value()) {
                return *slot.borrow_origin;
            }
        }
    }

    if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr) {
        if (unary->op == ast::UnaryOp::Move) {
            return borrowSourcePlace(state, *unary->operand);
        }
        if (unary->op == ast::UnaryOp::Borrow ||
            unary->op == ast::UnaryOp::BorrowMut) {
            auto place = resolvePlace(state, *unary->operand);
            if (!place) {
                return std::unexpected(place.error());
            }
            return *place;
        }
    }

    auto expr_type = analyzeExpr(state, expr);
    if (!expr_type) {
        return std::unexpected(expr_type.error());
    }
    if (!is_borrow_like_type(*expr_type)) {
        return unexpected_result<ast::ResolvedPlace>(
            "expected a borrow expression", expr.range);
    }

    if (const auto* name = std::get_if<ast::NameExpr>(&expr.node);
        name != nullptr) {
        const auto local_index = findLocalById(state, name->local_id);
        if (local_index.has_value()) {
            const auto& local = state.locals[*local_index];
            if (local.borrow_origin.has_value()) {
                return *local.borrow_origin;
            }
        }
    }

    if (expr.resolved_place.has_value()) {
        return *expr.resolved_place;
    }

    return unexpected_result<ast::ResolvedPlace>(
        "could not determine borrow source", expr.range);
}

auto SemanticAnalyzer::borrowSourceLocalId(FunctionState& state,
                                           ast::Expr& expr)
    -> std::expected<std::optional<std::size_t>, Diagnostic> {
    if (expr.cached_view_bindings.has_value()) {
        const auto it =
            std::ranges::find_if(*expr.cached_view_bindings,
                                 [](const ast::CachedViewBinding& binding) {
                                     return binding.path.empty();
                                 });
        if (it != expr.cached_view_bindings->end()) {
            return it->source_local_id;
        }
    }

    if (auto* name = std::get_if<ast::NameExpr>(&expr.node); name != nullptr) {
        if (name->local_id == 0) {
            const auto local_index = lookupLocal(state, name->name);
            if (local_index.has_value()) {
                name->local_id = state.locals[*local_index].unique_id;
            }
        }
        if (name->local_id != 0) {
            const auto local_index = findLocalById(state, name->local_id);
            if (local_index.has_value() &&
                is_borrow_like_type(state.locals[*local_index].type)) {
                return state.locals[*local_index].unique_id;
            }
        }
    }

    if (expr.resolved_place.has_value() && expr.resolved_type != nullptr &&
        is_borrow_like_type(expr.resolved_type)) {
        if (has_direct_interface_source(expr)) {
            return std::optional<std::size_t>{};
        }
        if (const auto slot_index = findViewSlotLocal(
                state, expr.resolved_place->is_external,
                expr.resolved_place->root_id, expr.resolved_place->fields);
            slot_index.has_value()) {
            return state.locals[*slot_index].unique_id;
        }
    }

    if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr) {
        if (unary->op == ast::UnaryOp::Move) {
            return borrowSourceLocalId(state, *unary->operand);
        }
        if (unary->op == ast::UnaryOp::Borrow ||
            unary->op == ast::UnaryOp::BorrowMut) {
            auto place = resolvePlace(state, *unary->operand);
            if (!place) {
                return std::unexpected(place.error());
            }
            if (place->owner_local_id.has_value()) {
                return *place->owner_local_id;
            }
            const auto* deref =
                std::get_if<ast::UnaryExpr>(&unary->operand->node);
            if (deref == nullptr || deref->op != ast::UnaryOp::Dereference) {
                return std::optional<std::size_t>{};
            }
            return borrowSourceLocalId(state, *deref->operand);
        }
    }

    auto expr_type = analyzeExpr(state, expr);
    if (!expr_type) {
        return std::unexpected(expr_type.error());
    }
    if (!is_borrow_like_type(*expr_type)) {
        return std::optional<std::size_t>{};
    }

    return std::visit(
        Overloaded{
            [&](ast::NameExpr& name)
                -> std::expected<std::optional<std::size_t>, Diagnostic> {
                const auto local_index = findLocalById(state, name.local_id);
                if (!local_index.has_value() ||
                    !is_borrow_like_type(state.locals[*local_index].type)) {
                    return std::optional<std::size_t>{};
                }
                return state.locals[*local_index].unique_id;
            },
            [&](ast::UnaryExpr& unary)
                -> std::expected<std::optional<std::size_t>, Diagnostic> {
                if (unary.op == ast::UnaryOp::Move) {
                    return borrowSourceLocalId(state, *unary.operand);
                }
                if (unary.op != ast::UnaryOp::Borrow &&
                    unary.op != ast::UnaryOp::BorrowMut) {
                    return std::optional<std::size_t>{};
                }
                const auto* deref =
                    std::get_if<ast::UnaryExpr>(&unary.operand->node);
                if (deref == nullptr ||
                    deref->op != ast::UnaryOp::Dereference) {
                    return std::optional<std::size_t>{};
                }
                return borrowSourceLocalId(state, *deref->operand);
            },
            [&](ast::CallExpr&)
                -> std::expected<std::optional<std::size_t>, Diagnostic> {
                return std::optional<std::size_t>{};
            },
            [&](auto&)
                -> std::expected<std::optional<std::size_t>, Diagnostic> {
                return std::optional<std::size_t>{};
            },
        },
        expr.node);
}

auto SemanticAnalyzer::declareLocal(FunctionState& state, std::string name,
                                    const Type* type, bool is_parameter,
                                    SourceRange range)
    -> std::expected<std::size_t, Diagnostic> {
    auto& scope = state.scopes.back().locals;
    if (scope.contains(name)) {
        return unexpected_result<std::size_t>(
            "duplicate local declaration for '" + name + "'", range);
    }

    const auto index = state.locals.size();
    scope.emplace(name, index);
    state.locals.push_back(
        LocalState{.name = std::move(name),
                   .type = type,
                   .in_scope = true,
                   .scope_depth = state.scopes.size() - 1,
                   .unique_id = next_local_id++,
                   .status = LocalState::Status::Uninitialized,
                   .borrow_origin = std::nullopt,
                   .element_origins = {},
                   .reborrow_parent_local_id = std::nullopt,
                   .is_parameter = is_parameter,
                   .is_hidden = false,
                   .is_view_slot = false,
                   .slot_root_is_external = false,
                   .slot_root_id = 0,
                   .slot_path = {}});
    return index;
}

auto SemanticAnalyzer::declareHiddenLocal(FunctionState& state,
                                          const Type* type) -> std::size_t {
    const auto index = state.locals.size();
    state.locals.push_back(LocalState{
        .name = "$hidden$" + std::to_string(next_local_id),
        .type = type,
        .in_scope = true,
        .scope_depth = state.scopes.size() - 1,
        .unique_id = next_local_id++,
        .status = LocalState::Status::Live,
        .borrow_origin = std::nullopt,
        .element_origins = {},
        .reborrow_parent_local_id = std::nullopt,
        .is_parameter = false,
        .is_hidden = true,
        .is_view_slot = false,
        .slot_root_is_external = false,
        .slot_root_id = 0,
        .slot_path = {},
    });
    return index;
}

auto SemanticAnalyzer::lookupLocal(FunctionState& state,
                                   std::string_view name) const
    -> std::optional<std::size_t> {
    for (auto scope_it = state.scopes.rbegin(); scope_it != state.scopes.rend();
         ++scope_it) {
        if (const auto it = scope_it->locals.find(std::string(name));
            it != scope_it->locals.end()) {
            return it->second;
        }
    }
    return std::nullopt;
}

auto SemanticAnalyzer::findLocalById(FunctionState& state,
                                     std::size_t local_id) const
    -> std::optional<std::size_t> {
    return findLocalById(std::as_const(state), local_id);
}

auto SemanticAnalyzer::findLocalById(const FunctionState& state,
                                     std::size_t local_id) const
    -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < state.locals.size(); ++index) {
        if (state.locals[index].unique_id == local_id) {
            return index;
        }
    }
    return std::nullopt;
}

auto SemanticAnalyzer::requireReadable(FunctionState& state, ast::Expr& expr)
    -> std::expected<const Type*, Diagnostic> {
    return analyzeExpr(state, expr);
}

auto SemanticAnalyzer::consumeValue(FunctionState& state, ast::Expr& expr,
                                    const Type* expected_type)
    -> std::expected<const Type*, Diagnostic> {
    auto expr_type = analyzeExpr(state, expr, expected_type);
    if (!expr_type) {
        return std::unexpected(expr_type.error());
    }
    if (!can_consume_value_type(types, *expr_type, expected_type)) {
        if ((*expr_type)->kind == TypeKind::Pointer &&
            expected_type->kind == TypeKind::Pointer &&
            state.unchecked_depth > 0) {
            expr.resolved_type = expected_type;
            expr.resolved_place.reset();
            return expected_type;
        }
        return unexpected_result<const Type*>(
            "type mismatch: expected " + types.describe(expected_type) +
                ", got " + types.describe(*expr_type),
            expr.range);
    }

    if (!expr.resolved_place.has_value()) {
        return *expr_type;
    }

    if (types.isCopy(*expr_type)) {
        return *expr_type;
    }

    auto place = *expr.resolved_place;
    if (!place.fields.empty()) {
        return unexpected_result<const Type*>("partial move is not supported",
                                              expr.range);
    }

    auto local_index = !place.is_external ? findLocalById(state, place.root_id)
                                          : std::optional<std::size_t>{};
    if (!local_index.has_value()) {
        return unexpected_result<const Type*>("move requires a local place",
                                              expr.range);
    }

    auto writable =
        ensureCanWrite(state, place, expr.range, place.owner_local_id);
    if (!writable) {
        return std::unexpected(writable.error());
    }

    state.locals[*local_index].status = LocalState::Status::Moved;
    markAggregateViewSlots(state, place, *expr_type, LocalState::Status::Moved);
    return *expr_type;
}

auto SemanticAnalyzer::ensureBorrowSourceType(FunctionState& state,
                                              ast::Expr& expr,
                                              const Type* target_type)
    -> std::expected<void, Diagnostic> {
    if (target_type == nullptr || target_type->kind != TypeKind::Borrow) {
        return make_error("invalid borrow target type", expr.range);
    }

    const Type* actual_type = expr.resolved_type;
    if (actual_type == nullptr) {
        auto analyzed = analyzeExpr(state, expr);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        actual_type = *analyzed;
    }

    const Type* source_type = nullptr;
    bool source_from_existing_borrow = false;
    bool source_is_mut_borrow = false;
    if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr && (unary->op == ast::UnaryOp::Borrow ||
                             unary->op == ast::UnaryOp::BorrowMut)) {
        const Type* operand_type = unary->operand->resolved_type;
        if (operand_type == nullptr) {
            auto analyzed_operand = analyzeExpr(state, *unary->operand);
            if (!analyzed_operand) {
                return std::unexpected(analyzed_operand.error());
            }
            operand_type = *analyzed_operand;
        }
        source_from_existing_borrow = operand_type->kind == TypeKind::Borrow;
        source_is_mut_borrow = unary->op == ast::UnaryOp::BorrowMut;
        source_type = source_from_existing_borrow ? operand_type->element_type
                                                  : operand_type;
    } else if (actual_type->kind == TypeKind::Borrow) {
        source_from_existing_borrow = true;
        source_type = actual_type->element_type;
        source_is_mut_borrow = actual_type->is_mut;
    } else {
        source_type = actual_type;
        source_is_mut_borrow = false;
    }

    if (source_type != nullptr &&
        (!target_type->is_mut || !source_from_existing_borrow ||
         source_is_mut_borrow) &&
        (!target_type->is_mut || !source_type->is_const) &&
        types.sameIgnoringTopLevelConst(source_type,
                                        target_type->element_type)) {
        return {};
    }

    return unexpected_result<void>("type mismatch: expected " +
                                       types.describe(target_type) + ", got " +
                                       types.describe(actual_type),
                                   expr.range);
}

auto SemanticAnalyzer::createNamedBorrow(FunctionState& state,
                                         std::size_t local_id,
                                         ast::Expr& initializer,
                                         const Type* target_type)
    -> std::expected<void, Diagnostic> {
    const auto local_index = findLocalById(state, local_id);
    if (!local_index.has_value()) {
        return make_error("invalid local id", initializer.range);
    }
    if (target_type != nullptr && target_type->kind == TypeKind::Interface) {
        auto analyzed = analyzeExpr(state, initializer, target_type);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        if (!types.isSame(*analyzed, target_type)) {
            return unexpected_result<void>(
                "type mismatch: expected " + types.describe(target_type) +
                    ", got " + types.describe(*analyzed),
                initializer.range);
        }
    }
    auto origin =
        borrowFromExpr(state, initializer, target_type->is_mut, false);
    if (!origin) {
        return std::unexpected(origin.error());
    }
    if (target_type != nullptr && target_type->kind == TypeKind::Borrow) {
        auto compatible =
            ensureBorrowSourceType(state, initializer, target_type);
        if (!compatible) {
            return std::unexpected(compatible.error());
        }
    }

    auto& local = state.locals[*local_index];
    auto outlives =
        ensureViewSourceOutlivesLocal(state, local, *origin, initializer.range);
    if (!outlives) {
        return std::unexpected(outlives.error());
    }
    releaseReborrowParent(state, local);
    local.borrow_origin = *origin;
    local.element_origins.clear();
    local.status = LocalState::Status::Live;
    auto attached = attachReborrowParent(state, local, initializer);
    if (!attached) {
        return std::unexpected(attached.error());
    }
    return {};
}

auto SemanticAnalyzer::assignNamedBorrow(FunctionState& state,
                                         std::size_t local_id, ast::Expr& value)
    -> std::expected<void, Diagnostic> {
    const auto local_index = findLocalById(state, local_id);
    if (!local_index.has_value()) {
        return make_error("invalid borrow target", value.range);
    }
    auto& local = state.locals[*local_index];
    if (local.type != nullptr && local.type->kind == TypeKind::Interface) {
        auto analyzed = analyzeExpr(state, value, local.type);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        if (!types.isSame(*analyzed, local.type)) {
            return unexpected_result<void>(
                "type mismatch: expected " + types.describe(local.type) +
                    ", got " + types.describe(*analyzed),
                value.range);
        }
    }
    auto origin = borrowFromExpr(state, value, local.type->is_mut, false);
    if (!origin) {
        return std::unexpected(origin.error());
    }
    if (local.type != nullptr && local.type->kind == TypeKind::Borrow) {
        auto compatible = ensureBorrowSourceType(state, value, local.type);
        if (!compatible) {
            return std::unexpected(compatible.error());
        }
    }
    auto outlives =
        ensureViewSourceOutlivesLocal(state, local, *origin, value.range);
    if (!outlives) {
        return std::unexpected(outlives.error());
    }
    releaseReborrowParent(state, local);
    local.status = LocalState::Status::Moved;
    local.borrow_origin.reset();
    local.element_origins.clear();
    local.borrow_origin = *origin;
    local.status = LocalState::Status::Live;
    auto attached = attachReborrowParent(state, local, value);
    if (!attached) {
        return std::unexpected(attached.error());
    }
    return {};
}

auto SemanticAnalyzer::ensureViewSourceLive(FunctionState& state,
                                            const LocalState& local,
                                            SourceRange range)
    -> std::expected<void, Diagnostic> {
    const auto sources = view_source_places(local);
    if (sources.empty()) {
        return {};
    }

    const auto kind = view_source_kind(types, local.type);
    for (const auto& source_place : sources) {
        if (source_place.is_external) {
            continue;
        }
        const auto source_index = findLocalById(state, source_place.root_id);
        if (!source_index.has_value()) {
            return make_error("invalid " + std::string(kind) + " source",
                              range);
        }

        const auto& source_local = state.locals[*source_index];
        if (!source_local.in_scope ||
            source_local.status != LocalState::Status::Live) {
            return make_error(std::string(kind) + " source is no longer live",
                              range);
        }
    }
    return {};
}

auto SemanticAnalyzer::ensureViewSourceOutlivesLocal(
    FunctionState& state, const LocalState& local,
    const std::optional<ast::ResolvedPlace>& source_place, SourceRange range)
    -> std::expected<void, Diagnostic> {
    std::vector<ast::ResolvedPlace> sources;
    if (source_place.has_value()) {
        sources.push_back(*source_place);
    } else {
        sources = view_source_places(local);
    }
    if (sources.empty()) {
        return {};
    }

    const auto kind = view_source_kind(types, local.type);
    const auto* target_local = &local;
    if (local.is_view_slot && !local.slot_root_is_external) {
        const auto target_root_index = findLocalById(state, local.slot_root_id);
        if (target_root_index.has_value()) {
            target_local = &state.locals[*target_root_index];
        }
    }
    for (const auto& source : sources) {
        if (source.is_external) {
            continue;
        }
        const auto source_index = findLocalById(state, source.root_id);
        if (!source_index.has_value()) {
            return make_error("invalid " + std::string(kind) + " source",
                              range);
        }

        const auto& source_local = state.locals[*source_index];
        if (!source_local.in_scope ||
            source_local.status != LocalState::Status::Live) {
            return make_error(std::string(kind) + " source is no longer live",
                              range);
        }
        const auto source_drops_after_target =
            source_local.is_hidden != target_local->is_hidden
                ? source_local.is_hidden && !target_local->is_hidden
                : source_local.unique_id < target_local->unique_id;
        if (source_local.scope_depth > target_local->scope_depth ||
            (source_local.scope_depth == target_local->scope_depth &&
             !source_drops_after_target)) {
            return make_error(std::string(kind) + " source does not live long "
                                                  "enough",
                              range);
        }
    }
    return {};
}

auto SemanticAnalyzer::borrowFromExpr(FunctionState& state, ast::Expr& expr,
                                      bool want_mut, bool temporary_only)
    -> std::expected<ast::ResolvedPlace, Diagnostic> {
    if (const auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr && (unary->op == ast::UnaryOp::Borrow ||
                             unary->op == ast::UnaryOp::BorrowMut)) {
        const auto explicit_mut = unary->op == ast::UnaryOp::BorrowMut;
        if (explicit_mut && !want_mut) {
            return unexpected_result<ast::ResolvedPlace>(
                "cannot bind '&mut' to an immutable borrow target", expr.range);
        }

        auto place = resolvePlace(state, *unary->operand);
        if (!place) {
            return std::unexpected(place.error());
        }

        const auto local_index = !place->is_external
                                     ? findLocalById(state, place->root_id)
                                     : std::optional<std::size_t>{};
        if (local_index.has_value()) {
            const auto& root = state.locals[*local_index];
            if (root.status != LocalState::Status::Live) {
                return unexpected_result<ast::ResolvedPlace>(
                    "cannot borrow from a moved or uninitialized value",
                    expr.range);
            }
        }
        auto operand_type = analyzeExpr(state, *unary->operand);
        if (!operand_type) {
            return std::unexpected(operand_type.error());
        }
        if (want_mut && (*operand_type)->is_const) {
            return unexpected_result<ast::ResolvedPlace>(
                "cannot mutably borrow a const place", expr.range);
        }

        auto borrow = ensureCanBorrow(state, *place, want_mut, expr.range,
                                      place->owner_local_id);
        if (!borrow) {
            return std::unexpected(borrow.error());
        }

        if (temporary_only) {
            state.temporary_loans.push_back(TemporaryLoan{*place, want_mut});
            if (want_mut && place->owner_local_id.has_value()) {
                const auto parent_index =
                    findLocalById(state, *place->owner_local_id);
                if (parent_index.has_value()) {
                    state.locals[*parent_index].status =
                        LocalState::Status::Moved;
                    state.temporary_suspended_local_ids.push_back(
                        *place->owner_local_id);
                }
            }
        }
        return *place;
    }

    const Type* expr_type = expr.resolved_type;
    if (expr_type == nullptr) {
        auto analyzed = analyzeExpr(state, expr);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        expr_type = *analyzed;
    }
    if (!is_borrow_like_type(expr_type)) {
        if (!expr.resolved_place.has_value()) {
            return unexpected_result<ast::ResolvedPlace>(
                "expected a borrow expression or borrow local", expr.range);
        }
        if (want_mut && expr_type->is_const) {
            return unexpected_result<ast::ResolvedPlace>(
                "cannot mutably borrow a const place", expr.range);
        }
        auto borrow =
            ensureCanBorrow(state, *expr.resolved_place, want_mut, expr.range,
                            expr.resolved_place->owner_local_id);
        if (!borrow) {
            return std::unexpected(borrow.error());
        }
        if (temporary_only) {
            state.temporary_loans.push_back(
                TemporaryLoan{*expr.resolved_place, want_mut});
            if (want_mut && expr.resolved_place->owner_local_id.has_value()) {
                const auto parent_index =
                    findLocalById(state, *expr.resolved_place->owner_local_id);
                if (parent_index.has_value()) {
                    state.locals[*parent_index].status =
                        LocalState::Status::Moved;
                    state.temporary_suspended_local_ids.push_back(
                        *expr.resolved_place->owner_local_id);
                }
            }
        }
        return *expr.resolved_place;
    }

    auto source_local_id = borrowSourceLocalId(state, expr);
    if (!source_local_id) {
        return std::unexpected(source_local_id.error());
    }

    if (!source_local_id->has_value()) {
        if (expr.resolved_place.has_value()) {
            auto borrow = ensureCanBorrow(state, *expr.resolved_place, want_mut,
                                          expr.range,
                                          expr.resolved_place->owner_local_id);
            if (!borrow) {
                return std::unexpected(borrow.error());
            }
            if (temporary_only) {
                state.temporary_loans.push_back(
                    TemporaryLoan{*expr.resolved_place, want_mut});
                if (want_mut &&
                    expr.resolved_place->owner_local_id.has_value()) {
                    const auto parent_index = findLocalById(
                        state, *expr.resolved_place->owner_local_id);
                    if (parent_index.has_value()) {
                        state.locals[*parent_index].status =
                            LocalState::Status::Moved;
                        state.temporary_suspended_local_ids.push_back(
                            *expr.resolved_place->owner_local_id);
                    }
                }
            }
            return *expr.resolved_place;
        }
        return unexpected_result<ast::ResolvedPlace>(
            "borrow values currently must come from named locals", expr.range);
    }

    const auto local_index = findLocalById(state, **source_local_id);
    if (!local_index.has_value()) {
        return unexpected_result<ast::ResolvedPlace>("invalid borrow source",
                                                     expr.range);
    }

    auto& local = state.locals[*local_index];
    if (!local.borrow_origin.has_value()) {
        return unexpected_result<ast::ResolvedPlace>(
            "borrow local is missing its origin", expr.range);
    }

    if (want_mut) {
        if (!local.type->is_mut) {
            return unexpected_result<ast::ResolvedPlace>(
                "expected a mutable borrow source", expr.range);
        }
        auto borrow = ensureCanBorrow(state, *local.borrow_origin, true,
                                      expr.range, local.unique_id);
        if (!borrow) {
            return std::unexpected(borrow.error());
        }
        if (temporary_only) {
            auto loan_place = *local.borrow_origin;
            loan_place.owner_local_id = local.unique_id;
            local.status = LocalState::Status::Moved;
            state.temporary_loans.push_back(TemporaryLoan{loan_place, true});
            state.temporary_suspended_local_ids.push_back(local.unique_id);
        }
        return *local.borrow_origin;
    }

    if (local.type->is_mut) {
        auto borrow = ensureCanBorrow(state, *local.borrow_origin, false,
                                      expr.range, local.unique_id);
        if (!borrow) {
            return std::unexpected(borrow.error());
        }
        if (temporary_only) {
            auto loan_place = *local.borrow_origin;
            loan_place.owner_local_id = local.unique_id;
            state.temporary_loans.push_back(TemporaryLoan{loan_place, false});
        }
        return *local.borrow_origin;
    }

    return *local.borrow_origin;
}

auto SemanticAnalyzer::resolvePlace(FunctionState& state, ast::Expr& expr)
    -> std::expected<ast::ResolvedPlace, Diagnostic> {
    if (expr.resolved_place.has_value() && expr.resolved_type != nullptr) {
        return *expr.resolved_place;
    }

    return std::visit(
        Overloaded{
            [&](ast::NameExpr& name)
                -> std::expected<ast::ResolvedPlace, Diagnostic> {
                const auto local_index = lookupLocal(state, name.name);
                if (!local_index.has_value()) {
                    return unexpected_result<ast::ResolvedPlace>(
                        "unknown identifier '" + name.name + "'", expr.range);
                }
                const auto& local = state.locals[*local_index];
                name.local_id = local.unique_id;
                expr.resolved_type = local.type;
                expr.resolved_place = localPlace(local.unique_id);
                return *expr.resolved_place;
            },
            [&](ast::MemberExpr&)
                -> std::expected<ast::ResolvedPlace, Diagnostic> {
                auto member_type = analyzeMember(
                    state, expr, std::get<ast::MemberExpr>(expr.node));
                if (!member_type) {
                    return std::unexpected(member_type.error());
                }
                return *expr.resolved_place;
            },
            [&](ast::IndexExpr&)
                -> std::expected<ast::ResolvedPlace, Diagnostic> {
                auto index_type = analyzeExpr(
                    state, expr,
                    std::get<ast::IndexExpr>(expr.node).base->resolved_type);
                if (!index_type) {
                    return std::unexpected(index_type.error());
                }
                return *expr.resolved_place;
            },
            [&](ast::UnaryExpr& unary)
                -> std::expected<ast::ResolvedPlace, Diagnostic> {
                if (unary.op != ast::UnaryOp::Dereference) {
                    return unexpected_result<ast::ResolvedPlace>(
                        "expression is not a place", expr.range);
                }
                auto deref_type = analyzeExpr(state, expr);
                if (!deref_type) {
                    return std::unexpected(deref_type.error());
                }
                if (!expr.resolved_place.has_value()) {
                    return unexpected_result<ast::ResolvedPlace>(
                        "dereference does not resolve to a borrow place",
                        expr.range);
                }
                return *expr.resolved_place;
            },
            [&](auto&) -> std::expected<ast::ResolvedPlace, Diagnostic> {
                return unexpected_result<ast::ResolvedPlace>(
                    "expression is not a place", expr.range);
            },
        },
        expr.node);
}

auto SemanticAnalyzer::ensureCanWrite(FunctionState& state,
                                      const ast::ResolvedPlace& place,
                                      SourceRange range,
                                      std::optional<std::size_t> ignored_local)
    -> std::expected<void, Diagnostic> {
    if (state.unchecked_depth > 0 && place.is_external) {
        return {};
    }

    for (const auto& loan : activeNamedLoans(state)) {
        if (!placesOverlap(place, loan.place)) {
            continue;
        }
        if (ignored_local.has_value() &&
            loan.place.owner_local_id == ignored_local) {
            continue;
        }
        return make_error("cannot write to a borrowed place", range);
    }

    for (const auto& loan : state.temporary_loans) {
        if (!placesOverlap(place, loan.place)) {
            continue;
        }
        if (ignored_local.has_value() &&
            loan.place.owner_local_id == ignored_local) {
            continue;
        }
        return make_error("cannot write to a temporarily borrowed place",
                          range);
    }

    if (!place.is_external) {
        const auto local_index = findLocalById(state, place.root_id);
        if (local_index.has_value()) {
            const auto& local = state.locals[*local_index];
            if (local.status != LocalState::Status::Live &&
                !place.fields.empty()) {
                return make_error(
                    "cannot access fields of a moved or uninitialized value",
                    range);
            }
        }
    }

    return {};
}

auto SemanticAnalyzer::ensureCanBorrow(FunctionState& state,
                                       const ast::ResolvedPlace& place,
                                       bool is_mut, SourceRange range,
                                       std::optional<std::size_t> ignored_local)
    -> std::expected<void, Diagnostic> {
    if (state.unchecked_depth > 0 && place.is_external) {
        return {};
    }

    auto check_loans =
        [&](const auto& loans) -> std::expected<void, Diagnostic> {
        for (const auto& loan : loans) {
            if (!placesOverlap(place, loan.place)) {
                continue;
            }
            if (ignored_local.has_value() &&
                loan.place.owner_local_id == ignored_local) {
                continue;
            }
            if (is_mut || loan.is_mut) {
                return make_error("borrow would violate aliasing rules", range);
            }
        }
        return {};
    };

    auto named = check_loans(activeNamedLoans(state));
    if (!named) {
        return std::unexpected(named.error());
    }
    auto temps = check_loans(state.temporary_loans);
    if (!temps) {
        return std::unexpected(temps.error());
    }
    return {};
}

auto SemanticAnalyzer::placesOverlap(const ast::ResolvedPlace& lhs,
                                     const ast::ResolvedPlace& rhs) const
    -> bool {
    if (lhs.root_id != rhs.root_id || lhs.is_external != rhs.is_external) {
        return false;
    }

    const auto min_size = std::min(lhs.fields.size(), rhs.fields.size());
    for (std::size_t index = 0; index < min_size; ++index) {
        if (lhs.fields[index] != rhs.fields[index]) {
            return false;
        }
    }
    return true;
}

auto SemanticAnalyzer::localPlace(std::size_t local_id) const
    -> ast::ResolvedPlace {
    return ast::ResolvedPlace{.root_id = local_id,
                              .is_external = false,
                              .owner_local_id = std::nullopt,
                              .fields = {}};
}

} // namespace sc

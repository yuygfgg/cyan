#include "sema_detail.hpp"

namespace cyan {

using namespace detail;

auto SemanticAnalyzer::branchMerge(FunctionState& into,
                                   const FunctionState& then_state,
                                   const FunctionState& else_state,
                                   SourceRange range, MergePolicy policy)
    -> std::expected<void, Diagnostic> {
    for (const auto& local : into.locals) {
        const auto then_index = findLocalById(then_state, local.unique_id);
        const auto else_index = findLocalById(else_state, local.unique_id);
        if (!then_index.has_value() || !else_index.has_value()) {
            continue;
        }

        const auto& then_local = then_state.locals[*then_index];
        const auto& else_local = else_state.locals[*else_index];
        if (then_local.status != else_local.status &&
            (policy == MergePolicy::Exact || !canJoinLocalStatus(local))) {
            return make_error(
                "control-flow merge requires identical local state for '" +
                    local.name + "'",
                range);
        }
        if (policy == MergePolicy::Exact &&
            topLevelOrigins(then_local) != topLevelOrigins(else_local)) {
            return make_error(
                "control-flow merge requires identical borrow origins for '" +
                    local.name + "'",
                range);
        }
        if (policy == MergePolicy::Exact &&
            then_local.element_origins != else_local.element_origins) {
            return make_error(
                "control-flow merge requires identical view element origins "
                "for '" +
                    local.name + "'",
                range);
        }
        if (then_local.reborrow_parent_local_id !=
            else_local.reborrow_parent_local_id) {
            return make_error(
                "control-flow merge requires identical reborrow state for '" +
                    local.name + "'",
                range);
        }
    }

    for (auto& local : into.locals) {
        const auto then_index = findLocalById(then_state, local.unique_id);
        const auto else_index = findLocalById(else_state, local.unique_id);
        if (!then_index.has_value() || !else_index.has_value()) {
            continue;
        }
        const auto& then_local = then_state.locals[*then_index];
        const auto& else_local = else_state.locals[*else_index];
        local.status =
            policy == MergePolicy::Exact
                ? then_local.status
                : joinLocalStatus(then_local.status, else_local.status);
        if (policy == MergePolicy::Exact) {
            setTopLevelOrigins(local, topLevelOrigins(then_local));
            local.element_origins = then_local.element_origins;
        } else {
            setTopLevelOrigins(local, joinPlaces(topLevelOrigins(then_local),
                                                 topLevelOrigins(else_local)));
            local.element_origins = joinPlaces(then_local.element_origins,
                                               else_local.element_origins);
        }
        local.reborrow_parent_local_id = then_local.reborrow_parent_local_id;
        local.in_scope = then_local.in_scope;
    }
    return {};
}

auto SemanticAnalyzer::mergeReachableStates(
    FunctionState& into, const std::vector<FunctionState>& states,
    SourceRange range) -> std::expected<void, Diagnostic> {
    if (states.empty()) {
        into.reachable = false;
        return {};
    }

    auto merged_state = states.front();
    for (std::size_t index = 1; index < states.size(); ++index) {
        auto merged = branchMerge(merged_state, merged_state, states[index],
                                  range, MergePolicy::Join);
        if (!merged) {
            return std::unexpected(merged.error());
        }
    }

    into.locals = merged_state.locals;
    into.view_slot_locals = merged_state.view_slot_locals;
    into.reachable = true;
    return {};
}

auto SemanticAnalyzer::appendContinueStates(FunctionState& into,
                                            const FunctionState& from) -> void {
    for (std::size_t loop_index = 0;
         loop_index < into.loops.size() && loop_index < from.loops.size();
         ++loop_index) {
        auto& into_continue_states = into.loops[loop_index].continue_states;
        const auto& from_continue_states =
            from.loops[loop_index].continue_states;
        into_continue_states.insert(into_continue_states.end(),
                                    from_continue_states.begin(),
                                    from_continue_states.end());
    }
}

auto SemanticAnalyzer::requireLoopBreakState(const FunctionState& state,
                                             SourceRange range)
    -> std::expected<void, Diagnostic> {
    if (state.loops.empty()) {
        return make_error("break must appear inside a loop", range);
    }

    const auto& entry_locals = state.loops.back().entry_locals;
    for (const auto& entry_local : entry_locals) {
        const auto current_index = findLocalById(state, entry_local.unique_id);
        if (!current_index.has_value()) {
            continue;
        }
        const auto& current_local = state.locals[*current_index];
        if (current_local.status != entry_local.status ||
            topLevelOrigins(current_local) != topLevelOrigins(entry_local) ||
            current_local.element_origins != entry_local.element_origins ||
            current_local.reborrow_parent_local_id !=
                entry_local.reborrow_parent_local_id) {
            return make_error("break is only allowed when loop entry "
                              "move/borrow state is restored",
                              range);
        }
    }
    return {};
}

auto SemanticAnalyzer::clearStatementTemporaries(FunctionState& state) -> void {
    auto has_live_reborrow_child = [&](std::size_t local_id) -> bool {
        return std::ranges::any_of(state.locals, [&](const LocalState& local) {
            return local.in_scope && isDefinitelyLive(local.status) &&
                   local.reborrow_parent_local_id == local_id;
        });
    };

    for (const auto local_id : state.temporary_suspended_local_ids) {
        const auto local_index = findLocalById(state, local_id);
        if (local_index.has_value() && state.locals[*local_index].in_scope &&
            !has_live_reborrow_child(local_id)) {
            state.locals[*local_index].status = LocalState::Status::Live;
        }
    }
    state.temporary_suspended_local_ids.clear();
    state.temporary_loans.clear();
}

auto SemanticAnalyzer::enterScope(FunctionState& state) -> void {
    state.scopes.emplace_back();
}

auto SemanticAnalyzer::releaseReborrowParent(FunctionState& state,
                                             LocalState& local) -> void {
    if (!local.reborrow_parent_local_id.has_value()) {
        return;
    }
    const auto parent_index =
        findLocalById(state, *local.reborrow_parent_local_id);
    if (parent_index.has_value() && state.locals[*parent_index].in_scope) {
        state.locals[*parent_index].status = LocalState::Status::Live;
    }
    local.reborrow_parent_local_id.reset();
}

auto SemanticAnalyzer::attachReborrowParent(FunctionState& state,
                                            LocalState& local,
                                            ast::Expr& source_expr)
    -> std::expected<void, Diagnostic> {
    releaseReborrowParent(state, local);
    if (local.type == nullptr || !is_borrow_like_type(local.type)) {
        return {};
    }

    auto source_local_id = borrowSourceLocalId(state, source_expr);
    if (!source_local_id) {
        return std::unexpected(source_local_id.error());
    }
    if (!source_local_id->has_value() || **source_local_id == local.unique_id) {
        return {};
    }

    const auto parent_index = findLocalById(state, **source_local_id);
    if (!parent_index.has_value()) {
        return make_error("invalid borrow source", source_expr.range);
    }

    auto& parent = state.locals[*parent_index];
    if (!is_borrow_like_type(parent.type) || !parent.type->is_mut) {
        return {};
    }

    if (!local.type->is_mut) {
        return {};
    }

    local.reborrow_parent_local_id = parent.unique_id;
    parent.status = LocalState::Status::Moved;
    return {};
}

auto SemanticAnalyzer::leaveScope(FunctionState& state) -> void {
    const auto depth = state.scopes.size() - 1;
    for (auto& local : state.locals) {
        if (local.scope_depth == depth && local.in_scope) {
            releaseReborrowParent(state, local);
            local.in_scope = false;
        }
    }
    state.scopes.pop_back();
}

auto SemanticAnalyzer::activeNamedLoans(const FunctionState& state) const
    -> std::vector<TemporaryLoan> {
    std::vector<TemporaryLoan> loans;
    for (const auto& local : state.locals) {
        if (!local.in_scope || !mayBeLive(local.status) ||
            (topLevelOrigins(local).empty() && local.element_origins.empty())) {
            continue;
        }
        const auto* local_type = types.unqualify(local.type);
        if (!is_borrow_like_type(local.type) &&
            local_type->kind != TypeKind::Slice) {
            continue;
        }
        if (!local.element_origins.empty()) {
            for (const auto& source_place : local.element_origins) {
                auto place = source_place;
                place.owner_local_id = local.unique_id;
                loans.push_back(TemporaryLoan{
                    .place = place,
                    .is_mut = false,
                });
            }
            continue;
        }
        for (auto place : topLevelOrigins(local)) {
            place.owner_local_id = local.unique_id;
            loans.push_back(TemporaryLoan{
                .place = std::move(place),
                .is_mut = is_borrow_like_type(local.type) && local.type->is_mut,
            });
        }
    }
    return loans;
}

auto SemanticAnalyzer::collectDropLocalIds(
    const FunctionState& state,
    const std::function<bool(const LocalState&)>& predicate) const
    -> std::vector<std::size_t> {
    std::vector<std::size_t> drop_local_ids;
    auto collect_matching = [&](bool include_hidden) {
        for (const auto& local : std::views::reverse(state.locals)) {
            if (!isDefinitelyLive(local.status) ||
                !types.needsDrop(local.type) || !predicate(local) ||
                local.is_hidden != include_hidden) {
                continue;
            }
            drop_local_ids.push_back(local.unique_id);
        }
    };
    collect_matching(false);
    collect_matching(true);
    return drop_local_ids;
}

} // namespace cyan

#include "sema_detail.hpp"

namespace cyan {

using namespace detail;

auto SemanticAnalyzer::analyzeFunction(ast::FunctionDecl& decl)
    -> std::expected<void, Diagnostic> {
    ScopedModule scoped_module(active_module, decl.owner_module);
    if (decl.is_extern) {
        return {};
    }

    struct RestoreLocalId {
        std::size_t& slot;
        std::size_t saved_value;

        ~RestoreLocalId() { slot = saved_value; }
    };

    const auto saved_next_local_id = next_local_id;
    RestoreLocalId restore_local_id{.slot = next_local_id,
                                    .saved_value = saved_next_local_id};

    FunctionState state;
    state.function = &decl;
    state.return_type = decl.resolved_return_type;
    state.scopes.emplace_back();
    next_local_id = 1;
    bool had_error = false;

    for (auto& parameter : decl.parameters) {
        auto declared =
            declareLocal(state, parameter.name, parameter.resolved_type, true,
                         parameter.range);
        if (!declared) {
            report(declared.error());
            had_error = true;
            continue;
        }
        const auto index = *declared;
        auto& local = state.locals[index];
        local.status = LocalState::Status::Live;
        parameter.local_id = local.unique_id;
        if (is_view_like_type(types, local.type)) {
            setTopLevelOrigins(local, {ast::ResolvedPlace{
                                          .root_id = next_external_root++,
                                          .is_external = true,
                                          .owner_local_id = std::nullopt,
                                          .fields = {},
                                      }});
            local.is_view_slot = true;
            local.slot_root_is_external = false;
            local.slot_root_id = local.unique_id;
            registerViewSlot(state, index);
            if (local.type->kind == TypeKind::Borrow &&
                local.type->element_type != nullptr &&
                typeContainsViews(local.type->element_type)) {
                const auto origin =
                    placeSetRepresentative(topLevelOrigins(local));
                if (!origin.has_value()) {
                    report(Diagnostic("borrow parameter is missing provenance",
                                      parameter.range));
                    had_error = true;
                    continue;
                }
                auto slots = declareAggregateViewSlots(
                    state, *origin, local.type->element_type, true,
                    LocalState::Status::Live);
                if (!slots) {
                    report(slots.error());
                    had_error = true;
                }
            }
        } else if (typeContainsViews(local.type)) {
            auto slots = declareAggregateViewSlots(
                state, localPlace(local.unique_id), local.type, true,
                LocalState::Status::Live);
            if (!slots) {
                report(slots.error());
                had_error = true;
            }
        }
    }

    if (decl.body == nullptr) {
        report(Diagnostic("function is missing a body", decl.range));
        return make_error("function is missing a body", decl.range);
    }

    enterScope(state);
    for (auto& statement : decl.body->statements) {
        if (!state.reachable) {
            break;
        }

        const auto saved_state = state;
        const auto statement_next_local_id = next_local_id;
        auto analyzed = analyzeStmt(state, *statement);
        if (!analyzed) {
            report(analyzed.error());
            state = saved_state;
            next_local_id = statement_next_local_id;
            had_error = true;
            clearStatementTemporaries(state);
            continue;
        }
        clearStatementTemporaries(state);
    }
    if (state.reachable) {
        const auto current_depth = state.scopes.size() - 1;
        decl.body->exit_drop_local_ids =
            collectDropLocalIds(state, [&](const LocalState& local) {
                return local.in_scope && local.scope_depth == current_depth;
            });
    } else {
        decl.body->exit_drop_local_ids.clear();
    }
    leaveScope(state);

    decl.exit_drop_local_ids = collectDropLocalIds(
        state, [](const LocalState& local) { return local.in_scope; });
    if (state.reachable &&
        types.unqualify(decl.resolved_return_type) == types.voidType()) {
        auto validated_outputs =
            validateMutableParameterDependencies(state, decl.range);
        if (!validated_outputs) {
            report(validated_outputs.error());
            had_error = true;
        }
    }
    if (state.reachable &&
        types.unqualify(decl.resolved_return_type) != types.voidType()) {
        report(Diagnostic("missing return in non-void function", decl.range));
        had_error = true;
    }
    if (had_error) {
        return make_error("function analysis failed", decl.range);
    }
    return {};
}

auto SemanticAnalyzer::analyzeBlock(FunctionState& state, ast::Block& block)
    -> std::expected<void, Diagnostic> {
    enterScope(state);
    for (auto& statement : block.statements) {
        if (!state.reachable) {
            break;
        }
        auto analyzed = analyzeStmt(state, *statement);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        clearStatementTemporaries(state);
    }
    if (state.reachable) {
        const auto current_depth = state.scopes.size() - 1;
        block.exit_drop_local_ids =
            collectDropLocalIds(state, [&](const LocalState& local) {
                return local.in_scope && local.scope_depth == current_depth;
            });
    } else {
        block.exit_drop_local_ids.clear();
    }
    leaveScope(state);
    return {};
}

auto SemanticAnalyzer::analyzeStmt(FunctionState& state, ast::Stmt& stmt)
    -> std::expected<void, Diagnostic> {
    return std::visit(
        Overloaded{
            [&](ast::VarDeclStmt& var_decl) {
                return analyzeVarDecl(state, var_decl);
            },
            [&](ast::ExprStmt& expr_stmt) {
                return analyzeExprStmt(state, expr_stmt);
            },
            [&](ast::AssignStmt& assign) {
                return analyzeAssign(state, assign);
            },
            [&](ast::UpdateStmt& update) {
                return analyzeUpdate(state, update);
            },
            [&](ast::ReturnStmt& ret) { return analyzeReturn(state, ret); },
            [&](ast::DropStmt& drop) { return analyzeDrop(state, drop); },
            [&](ast::IfStmt& if_stmt) { return analyzeIf(state, if_stmt); },
            [&](ast::WhileStmt& while_stmt) {
                return analyzeWhile(state, while_stmt);
            },
            [&](ast::ForStmt& for_stmt) { return analyzeFor(state, for_stmt); },
            [&](ast::BreakStmt& break_stmt) {
                return analyzeBreak(state, break_stmt);
            },
            [&](ast::ContinueStmt& continue_stmt) {
                return analyzeContinue(state, continue_stmt);
            },
            [&](ast::UncheckedStmt& unchecked_stmt) {
                return analyzeUnchecked(state, unchecked_stmt);
            },
            [&](ast::SwitchStmt& switch_stmt) {
                return analyzeSwitch(state, switch_stmt);
            },
            [&](ast::Block& nested) { return analyzeBlock(state, nested); },
        },
        stmt.node);
}

auto SemanticAnalyzer::analyzeVarDecl(FunctionState& state,
                                      ast::VarDeclStmt& stmt)
    -> std::expected<void, Diagnostic> {
    auto resolved_type = resolveType(*stmt.type);
    if (!resolved_type) {
        return std::unexpected(resolved_type.error());
    }

    auto declared = declareLocal(
        state, stmt.name, *resolved_type, false,
        SourceRange{.begin = stmt.type->range.begin,
                    .end = stmt.initializer ? stmt.initializer->range.end
                                            : stmt.type->range.end,
                    .source = stmt.type->range.source});
    if (!declared) {
        return std::unexpected(declared.error());
    }

    const auto local_id = state.locals[*declared].unique_id;
    stmt.local_id = local_id;
    if (is_view_like_type(types, *resolved_type)) {
        auto& local = state.locals[*declared];
        local.is_view_slot = true;
        local.slot_root_is_external = false;
        local.slot_root_id = local.unique_id;
        registerViewSlot(state, *declared);
    } else if (typeContainsViews(*resolved_type)) {
        auto slots = declareAggregateViewSlots(
            state, localPlace(local_id), *resolved_type, false,
            LocalState::Status::Uninitialized);
        if (!slots) {
            return std::unexpected(slots.error());
        }
    }

    if (stmt.initializer == nullptr) {
        if (is_borrow_like_type(*resolved_type)) {
            return make_error("borrow locals must be initialized",
                              stmt.type->range);
        }
        return {};
    }

    if (is_borrow_like_type(*resolved_type)) {
        auto borrow = createNamedBorrow(state, stmt.local_id, *stmt.initializer,
                                        *resolved_type);
        if (!borrow) {
            return std::unexpected(borrow.error());
        }
    } else {
        std::vector<ViewLeafBinding> view_bindings;
        if (typeContainsViews(*resolved_type)) {
            auto analyzed_initializer =
                analyzeExpr(state, *stmt.initializer, *resolved_type);
            if (!analyzed_initializer) {
                return std::unexpected(analyzed_initializer.error());
            }
            auto bindings = collectExprViewBindings(state, *stmt.initializer);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            view_bindings = std::move(*bindings);
        }
        auto value = consumeValue(state, *stmt.initializer, *resolved_type);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto& local = state.locals[*declared];
        local.status = LocalState::Status::Live;
        if (typeContainsViews(*resolved_type)) {
            auto assigned = setAggregateViewSlots(state, localPlace(local_id),
                                                  *resolved_type, view_bindings,
                                                  stmt.initializer->range);
            if (!assigned) {
                return std::unexpected(assigned.error());
            }
        }
    }

    return {};
}

auto SemanticAnalyzer::analyzeAssign(FunctionState& state,
                                     ast::AssignStmt& stmt)
    -> std::expected<void, Diagnostic> {
    auto place = resolvePlace(state, *stmt.target);
    if (!place) {
        return std::unexpected(place.error());
    }
    const auto* const target_type = stmt.target->resolved_type;

    if (target_type == nullptr) {
        return make_error("invalid assignment target", stmt.target->range);
    }
    if (target_type->is_const) {
        return make_error("cannot assign to a const place", stmt.target->range);
    }

    const auto local_index = !place->is_external
                                 ? findLocalById(state, place->root_id)
                                 : std::optional<std::size_t>{};
    const auto slot_index = findViewSlotLocal(state, place->is_external,
                                              place->root_id, place->fields);
    auto write_targets = projectedPlaceTargets(state, *stmt.target);
    if (!write_targets) {
        return std::unexpected(write_targets.error());
    }
    if (!place->is_external && place->fields.empty() &&
        types.unqualify(target_type)->kind == TypeKind::Slice) {
        *write_targets = std::vector<ast::ResolvedPlace>{*place};
    } else if (write_targets->empty()) {
        *write_targets = std::vector<ast::ResolvedPlace>{*place};
    }
    auto write_ignored_local = [&](const ast::ResolvedPlace& write_target)
        -> std::optional<std::size_t> {
        if (place->owner_local_id.has_value()) {
            return place->owner_local_id;
        }
        if (write_target.owner_local_id.has_value()) {
            return write_target.owner_local_id;
        }
        if (!place->is_external && place->fields.empty()) {
            const auto owner_index = findLocalById(state, place->root_id);
            if (owner_index.has_value()) {
                const auto& owner_local = state.locals[*owner_index];
                if (owner_local.type != nullptr &&
                    types.unqualify(owner_local.type)->kind ==
                        TypeKind::Slice) {
                    return owner_local.unique_id;
                }
            }
        }
        if (!place->is_external && !place->fields.empty()) {
            const auto owner_index = findLocalById(state, place->root_id);
            if (owner_index.has_value()) {
                const auto& owner_local = state.locals[*owner_index];
                if (is_borrow_like_type(owner_local.type) &&
                    owner_local.type->is_mut) {
                    return owner_local.unique_id;
                }
            }
        }
        return std::nullopt;
    };
    if (slot_index.has_value() && is_borrow_like_type(target_type) &&
        !place->is_external && !place->fields.empty()) {
        for (const auto& write_target : *write_targets) {
            auto writable =
                ensureCanWrite(state, write_target, stmt.target->range,
                               write_ignored_local(write_target));
            if (!writable) {
                return std::unexpected(writable.error());
            }
        }
        if (local_index.has_value() &&
            !isDefinitelyLive(state.locals[*local_index].status)) {
            return make_error(
                "cannot write through a moved or uninitialized base value",
                stmt.target->range);
        }
        auto& slot = state.locals[*slot_index];
        auto assigned = assignNamedBorrow(state, slot.unique_id, *stmt.value);
        if (!assigned) {
            return std::unexpected(assigned.error());
        }
        return {};
    }
    if (local_index.has_value()) {
        auto& local = state.locals[*local_index];
        if (is_borrow_like_type(target_type) && place->fields.empty() &&
            !place->is_external) {
            auto writable = ensureCanWrite(state, *place, stmt.target->range,
                                           place->owner_local_id);
            if (!writable) {
                return std::unexpected(writable.error());
            }
            auto assigned =
                assignNamedBorrow(state, local.unique_id, *stmt.value);
            if (!assigned) {
                return std::unexpected(assigned.error());
            }
            return {};
        }
    }

    for (const auto& write_target : *write_targets) {
        auto writable = ensureCanWrite(state, write_target, stmt.target->range,
                                       write_ignored_local(write_target));
        if (!writable) {
            return std::unexpected(writable.error());
        }
    }

    auto root_was_live_before_assign = false;
    if (local_index.has_value()) {
        auto& root = state.locals[*local_index];
        root_was_live_before_assign =
            place->fields.empty() && isDefinitelyLive(root.status);
        if (root_was_live_before_assign && types.needsDrop(target_type) &&
            typeContainsViews(target_type)) {
            auto live = ensureViewSubtreeLive(state, *place, target_type,
                                              stmt.target->range);
            if (!live) {
                return std::unexpected(live.error());
            }
        }
        if (!isDefinitelyLive(root.status)) {
            if (!place->fields.empty()) {
                return make_error(
                    "cannot write through a moved or uninitialized base value",
                    stmt.target->range);
            }
        }
    }

    std::vector<ViewLeafBinding> view_bindings;
    if (typeContainsViews(target_type)) {
        auto analyzed_value = analyzeExpr(state, *stmt.value, target_type);
        if (!analyzed_value) {
            return std::unexpected(analyzed_value.error());
        }
        auto bindings = collectExprViewBindings(state, *stmt.value);
        if (!bindings) {
            return std::unexpected(bindings.error());
        }
        view_bindings = std::move(*bindings);
    }

    auto value = consumeValue(state, *stmt.value, target_type);
    if (!value) {
        return std::unexpected(value.error());
    }

    if (local_index.has_value() && place->fields.empty()) {
        const auto root_still_holds_old_value =
            isDefinitelyLive(state.locals[*local_index].status);
        stmt.drop_old_value = root_was_live_before_assign &&
                              root_still_holds_old_value &&
                              types.needsDrop(target_type);
        state.locals[*local_index].status = LocalState::Status::Live;
        if (typeContainsViews(target_type)) {
            auto assigned = setAggregateViewSlots(
                state, *place, target_type, view_bindings, stmt.value->range);
            if (!assigned) {
                return std::unexpected(assigned.error());
            }
        }
    } else if (typeContainsViews(target_type)) {
        auto assigned = setAggregateViewSlots(state, *place, target_type,
                                              view_bindings, stmt.value->range);
        if (!assigned) {
            return std::unexpected(assigned.error());
        }
    }

    return {};
}

auto SemanticAnalyzer::analyzeUpdate(FunctionState& state,
                                     ast::UpdateStmt& stmt)
    -> std::expected<void, Diagnostic> {
    auto target_type = requireReadable(state, *stmt.target);
    if (!target_type) {
        return std::unexpected(target_type.error());
    }

    auto place = resolvePlace(state, *stmt.target);
    if (!place) {
        return std::unexpected(place.error());
    }
    if ((*target_type)->is_const) {
        return make_error("cannot assign to a const place", stmt.target->range);
    }

    const auto local_index = !place->is_external
                                 ? findLocalById(state, place->root_id)
                                 : std::optional<std::size_t>{};

    auto write_targets = projectedPlaceTargets(state, *stmt.target);
    if (!write_targets) {
        return std::unexpected(write_targets.error());
    }
    if (write_targets->empty()) {
        *write_targets = std::vector<ast::ResolvedPlace>{*place};
    }
    auto write_ignored_local = [&](const ast::ResolvedPlace& write_target)
        -> std::optional<std::size_t> {
        if (place->owner_local_id.has_value()) {
            return place->owner_local_id;
        }
        if (write_target.owner_local_id.has_value()) {
            return write_target.owner_local_id;
        }
        if (!place->is_external && !place->fields.empty()) {
            const auto owner_index = findLocalById(state, place->root_id);
            if (owner_index.has_value()) {
                const auto& owner_local = state.locals[*owner_index];
                if (is_borrow_like_type(owner_local.type) &&
                    owner_local.type->is_mut) {
                    return owner_local.unique_id;
                }
            }
        }
        return std::nullopt;
    };
    for (const auto& write_target : *write_targets) {
        auto writable = ensureCanWrite(state, write_target, stmt.target->range,
                                       write_ignored_local(write_target));
        if (!writable) {
            return std::unexpected(writable.error());
        }
    }

    if (local_index.has_value()) {
        const auto& local = state.locals[*local_index];
        if (!isDefinitelyLive(local.status)) {
            if (place->fields.empty()) {
                return make_error(
                    "cannot update a moved or uninitialized value",
                    stmt.target->range);
            }
            return make_error(
                "cannot update through a moved or uninitialized base value",
                stmt.target->range);
        }
    }

    const auto* value_target_type = types.unqualify(*target_type);
    if (types.isInteger(value_target_type) ||
        types.isFloat(value_target_type)) {
        return {};
    }

    if (value_target_type->kind == TypeKind::Pointer &&
        state.unchecked_depth > 0 &&
        types.unqualify(value_target_type->element_type) != types.voidType()) {
        return {};
    }

    return make_error("increment/decrement requires an integer, "
                      "floating-point, or unchecked non-void pointer target",
                      stmt.target->range);
}

auto SemanticAnalyzer::validateMutableParameterDependencies(
    FunctionState& state, SourceRange range)
    -> std::expected<void, Diagnostic> {
    auto find_binding =
        [](std::vector<ViewLeafBinding>& bindings,
           const std::vector<std::uint32_t>& path) -> ViewLeafBinding* {
        const auto it =
            std::ranges::find_if(bindings, [&](const ViewLeafBinding& binding) {
                return binding.path == path;
            });
        return it == bindings.end() ? nullptr : &(*it);
    };
    auto binding_places =
        [&](const ViewLeafBinding& binding) -> std::vector<ast::ResolvedPlace> {
        if (binding.type != nullptr &&
            is_direct_shared_view_slice(types, binding.type) &&
            !binding.element_sources.empty()) {
            return binding.element_sources;
        }
        if (!binding.source_places.empty()) {
            return binding.source_places;
        }
        return {};
    };

    std::unordered_map<std::size_t, std::vector<ViewLeafBinding>>
        cached_parameter_bindings;
    std::vector<bool> include_projected_parameter_bindings(
        state.function->parameters.size(), false);
    for (const auto& dependency : state.function->return_dependencies) {
        if (dependency.target.is_return ||
            !dependency.source.parameter_index.has_value() ||
            dependency.source.resolved_path.empty()) {
            continue;
        }
        include_projected_parameter_bindings[*dependency.source
                                                  .parameter_index] = true;
    }
    auto parameter_bindings = [&](std::size_t parameter_index)
        -> std::expected<std::vector<ViewLeafBinding>*, Diagnostic> {
        if (!cached_parameter_bindings.contains(parameter_index)) {
            const auto& parameter = state.function->parameters[parameter_index];
            auto bindings = collectSlotBindings(
                state, localPlace(parameter.local_id), parameter.resolved_type);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            if (include_projected_parameter_bindings[parameter_index] &&
                parameter.resolved_type != nullptr &&
                parameter.resolved_type->kind == TypeKind::Borrow &&
                parameter.resolved_type->element_type != nullptr) {
                const auto local_index =
                    findLocalById(state, parameter.local_id);
                if (!local_index.has_value()) {
                    return unexpected_result<std::vector<ViewLeafBinding>*>(
                        "invalid depends source parameter", range);
                }
                auto projected_bindings = collectProjectedViewBindings(
                    state, topLevelOrigins(state.locals[*local_index]),
                    parameter.resolved_type->element_type);
                if (!projected_bindings) {
                    return std::unexpected(projected_bindings.error());
                }
                bindings->insert(
                    bindings->end(),
                    std::make_move_iterator(projected_bindings->begin()),
                    std::make_move_iterator(projected_bindings->end()));
            }
            cached_parameter_bindings.emplace(parameter_index,
                                              std::move(*bindings));
        }
        return &cached_parameter_bindings.at(parameter_index);
    };

    for (const auto& dependency : state.function->return_dependencies) {
        if (dependency.target.is_return ||
            !dependency.target.parameter_index.has_value() ||
            !dependency.source.parameter_index.has_value()) {
            continue;
        }

        auto source_bindings =
            parameter_bindings(*dependency.source.parameter_index);
        if (!source_bindings) {
            return std::unexpected(source_bindings.error());
        }

        const auto& target_parameter =
            state.function->parameters[*dependency.target.parameter_index];
        const auto target_local_index =
            findLocalById(state, target_parameter.local_id);
        if (!target_local_index.has_value()) {
            return make_error("invalid depends target parameter", range);
        }

        auto target_origin = placeSetRepresentative(
            topLevelOrigins(state.locals[*target_local_index]));
        if (!target_origin.has_value()) {
            return make_error("depends target parameter is missing provenance",
                              range);
        }
        target_origin->fields.insert(target_origin->fields.end(),
                                     dependency.target.resolved_path.begin(),
                                     dependency.target.resolved_path.end());

        auto actual_bindings = collectSlotBindings(
            state, *target_origin, dependency.target.resolved_type);
        if (!actual_bindings) {
            return std::unexpected(actual_bindings.error());
        }

        const auto target_leaves =
            collectViewLeafInfos(dependency.target.resolved_type);
        const auto source_leaves =
            collectViewLeafInfos(dependency.source.resolved_type);
        for (std::size_t leaf_index = 0; leaf_index < target_leaves.size();
             ++leaf_index) {
            auto source_path = dependency.source.resolved_path;
            source_path.insert(source_path.end(),
                               source_leaves[leaf_index].path.begin(),
                               source_leaves[leaf_index].path.end());
            auto* source_binding = find_binding(**source_bindings, source_path);
            if (source_binding == nullptr) {
                return make_error("depends source path is missing a tracked "
                                  "borrow or slice leaf",
                                  range);
            }

            auto* actual_binding =
                find_binding(*actual_bindings, target_leaves[leaf_index].path);
            if (actual_binding == nullptr) {
                return make_error("depends target path is missing a tracked "
                                  "borrow or slice leaf",
                                  range);
            }

            const auto actual_places = binding_places(*actual_binding);
            const auto expected_places = binding_places(*source_binding);
            if (actual_places.empty()) {
                return make_error("mutable parameter borrow or slice has no "
                                  "tracked source",
                                  range);
            }
            const auto every_actual_is_covered = std::ranges::all_of(
                actual_places, [&](const ast::ResolvedPlace& actual_place) {
                    return std::ranges::any_of(
                        expected_places,
                        [&](const ast::ResolvedPlace& expected_place) {
                            return is_same_or_subplace(actual_place,
                                                       expected_place);
                        });
                });
            if (expected_places.empty() || !every_actual_is_covered) {
                return make_error("mutable parameter borrow or slice does not "
                                  "match its declared depends source",
                                  range);
            }
        }
    }

    return {};
}

auto SemanticAnalyzer::analyzeReturn(FunctionState& state,
                                     ast::ReturnStmt& stmt)
    -> std::expected<void, Diagnostic> {
    if (types.unqualify(state.return_type) == types.voidType()) {
        if (stmt.value != nullptr) {
            return make_error("void function cannot return a value",
                              stmt.value->range);
        }
        auto validated_outputs =
            validateMutableParameterDependencies(state, state.function->range);
        if (!validated_outputs) {
            return std::unexpected(validated_outputs.error());
        }
        stmt.drop_local_ids = collectDropLocalIds(
            state, [](const LocalState& local) { return local.in_scope; });
        state.reachable = false;
        return {};
    }

    if (stmt.value == nullptr) {
        return make_error("non-void function must return a value",
                          state.function->range);
    }

    std::unordered_map<std::size_t, std::vector<ViewLeafBinding>>
        cached_return_source_parameters;
    std::vector<ViewLeafBinding> actual_bindings;
    if (typeContainsViews(state.return_type)) {
        std::vector<bool> include_projected_parameter_bindings(
            state.function->parameters.size(), false);
        for (const auto& dependency : state.function->return_dependencies) {
            if (!dependency.target.is_return ||
                !dependency.source.parameter_index.has_value() ||
                dependency.source.resolved_path.empty()) {
                continue;
            }
            include_projected_parameter_bindings[*dependency.source
                                                      .parameter_index] = true;
        }
        auto capture_return_source_parameters =
            [&](bool overwrite_existing,
                bool skip_unavailable) -> std::expected<void, Diagnostic> {
            for (const auto& dependency : state.function->return_dependencies) {
                if (!dependency.target.is_return ||
                    !dependency.source.parameter_index.has_value()) {
                    continue;
                }
                const auto parameter_index = *dependency.source.parameter_index;
                if (!overwrite_existing &&
                    cached_return_source_parameters.contains(parameter_index)) {
                    continue;
                }

                const auto& parameter =
                    state.function->parameters[parameter_index];
                const auto local_index =
                    findLocalById(state, parameter.local_id);
                if (!local_index.has_value()) {
                    return unexpected_result<void>(
                        "invalid depends source parameter", stmt.value->range);
                }
                auto bindings =
                    collectSlotBindings(state, localPlace(parameter.local_id),
                                        parameter.resolved_type);
                if (!bindings) {
                    if (skip_unavailable) {
                        const auto& message = bindings.error().message();
                        if (message == "cannot use a moved view value" ||
                            message ==
                                "cannot use an uninitialized view value" ||
                            message ==
                                "view value may be moved or uninitialized due "
                                "to control flow" ||
                            message ==
                                "cannot use a view value whose source is "
                                "unknown after this call or control-flow "
                                "path") {
                            continue;
                        }
                    }
                    return std::unexpected(bindings.error());
                }
                if (include_projected_parameter_bindings[parameter_index] &&
                    parameter.resolved_type != nullptr &&
                    parameter.resolved_type->kind == TypeKind::Borrow &&
                    parameter.resolved_type->element_type != nullptr) {
                    auto projected_bindings = collectProjectedViewBindings(
                        state, topLevelOrigins(state.locals[*local_index]),
                        parameter.resolved_type->element_type);
                    if (!projected_bindings) {
                        return std::unexpected(projected_bindings.error());
                    }
                    bindings->insert(
                        bindings->end(),
                        std::make_move_iterator(projected_bindings->begin()),
                        std::make_move_iterator(projected_bindings->end()));
                }

                if (overwrite_existing) {
                    cached_return_source_parameters[parameter_index] =
                        std::move(*bindings);
                } else {
                    cached_return_source_parameters.emplace(
                        parameter_index, std::move(*bindings));
                }
            }
            return {};
        };

        auto cached_sources_before_value =
            capture_return_source_parameters(false, false);
        if (!cached_sources_before_value) {
            return std::unexpected(cached_sources_before_value.error());
        }

        auto analyzed_value =
            analyzeExpr(state, *stmt.value, state.return_type);
        if (!analyzed_value) {
            return std::unexpected(analyzed_value.error());
        }
        auto bindings = collectExprViewBindings(state, *stmt.value);
        if (!bindings) {
            return std::unexpected(bindings.error());
        }
        actual_bindings = std::move(*bindings);

        auto refreshed_sources = capture_return_source_parameters(true, true);
        if (!refreshed_sources) {
            return std::unexpected(refreshed_sources.error());
        }
    }

    if (is_borrow_like_type(state.return_type)) {
        auto value_type = analyzeExpr(state, *stmt.value, state.return_type);
        if (!value_type) {
            return std::unexpected(value_type.error());
        }
        if (!types.isSame(*value_type, state.return_type)) {
            return make_error("return type does not match function signature",
                              stmt.value->range);
        }
    } else {
        auto value = consumeValue(state, *stmt.value, state.return_type);
        if (!value) {
            return std::unexpected(value.error());
        }
    }

    if (typeContainsViews(state.return_type)) {
        std::vector<ViewLeafBinding> expected_bindings;

        auto find_binding =
            [](std::vector<ViewLeafBinding>& bindings,
               const std::vector<std::uint32_t>& path) -> ViewLeafBinding* {
            const auto it = std::ranges::find_if(
                bindings, [&](const ViewLeafBinding& binding) {
                    return binding.path == path;
                });
            return it == bindings.end() ? nullptr : &(*it);
        };
        const auto return_dependency_count =
            std::ranges::count_if(state.function->return_dependencies,
                                  [](const ast::ReturnDependency& dependency) {
                                      return dependency.target.is_return;
                                  });
        auto resolve_shared_slice_sources =
            [&](const ViewLeafBinding& source_binding, const Type* source_type)
            -> std::expected<std::vector<ast::ResolvedPlace>, Diagnostic> {
            std::vector<ast::ResolvedPlace> sources =
                source_binding.element_sources;
            const auto* source_base =
                source_type == nullptr ? nullptr : types.unqualify(source_type);
            const auto source_is_borrowed_slice =
                source_base != nullptr &&
                source_base->kind == TypeKind::Borrow &&
                source_base->element_type != nullptr &&
                types.unqualify(source_base->element_type)->kind ==
                    TypeKind::Slice;
            if (!source_is_borrowed_slice || !sources.empty()) {
                return sources;
            }

            for (const auto& source_place : source_binding.source_places) {
                auto pointee_bindings = collectSlotBindings(
                    state, source_place, source_base->element_type);
                if (!pointee_bindings) {
                    return std::unexpected(pointee_bindings.error());
                }
                const auto pointee_it = std::ranges::find_if(
                    *pointee_bindings, [](const ViewLeafBinding& binding) {
                        return binding.path.empty();
                    });
                if (pointee_it == pointee_bindings->end()) {
                    continue;
                }
                const auto& pointee_sources =
                    !pointee_it->element_sources.empty()
                        ? pointee_it->element_sources
                        : pointee_it->source_places;
                for (const auto& pointee_source : pointee_sources) {
                    if (std::ranges::find(sources, pointee_source) ==
                        sources.end()) {
                        sources.push_back(pointee_source);
                    }
                }
            }
            return sources;
        };

        for (const auto& dependency : state.function->return_dependencies) {
            if (!dependency.target.is_return) {
                continue;
            }
            if (!dependency.source.parameter_index.has_value()) {
                continue;
            }
            const auto parameter_index = *dependency.source.parameter_index;
            auto& parameter_bindings =
                cached_return_source_parameters.at(parameter_index);
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
                    find_binding(parameter_bindings, source_path);
                if (source_binding == nullptr) {
                    return make_error("depends source path is missing a "
                                      "tracked borrow or slice leaf",
                                      stmt.value->range);
                }
                auto source_element_sources = source_binding->element_sources;
                if (is_direct_shared_view_slice(types,
                                                target_leaves[index].type)) {
                    auto shared_sources = resolve_shared_slice_sources(
                        *source_binding, source_leaves[index].type);
                    if (!shared_sources) {
                        return std::unexpected(shared_sources.error());
                    }
                    source_element_sources = std::move(*shared_sources);
                }
                auto target_path = dependency.target.resolved_path;
                target_path.insert(target_path.end(),
                                   target_leaves[index].path.begin(),
                                   target_leaves[index].path.end());
                expected_bindings.push_back(ViewLeafBinding{
                    .path = std::move(target_path),
                    .source_places = source_binding->source_places,
                    .source_local_id = source_binding->source_local_id,
                    .element_sources = std::move(source_element_sources),
                    .type = target_leaves[index].type,
                });
            }
        }

        auto binding_places = [&](const ViewLeafBinding& binding)
            -> std::vector<ast::ResolvedPlace> {
            if (binding.type != nullptr &&
                is_direct_shared_view_slice(types, binding.type) &&
                !binding.element_sources.empty()) {
                return binding.element_sources;
            }
            if (!binding.source_places.empty()) {
                return binding.source_places;
            }
            return {};
        };

        for (auto& actual_binding : actual_bindings) {
            auto* expected_binding =
                find_binding(expected_bindings, actual_binding.path);
            if (expected_binding == nullptr) {
                return make_error("returned borrow or slice path is not "
                                  "covered by depends clause",
                                  stmt.value->range);
            }
            const auto actual_places = binding_places(actual_binding);
            const auto expected_places = binding_places(*expected_binding);
            if (actual_places.empty()) {
                return make_error(
                    "returned borrow or slice has no tracked source",
                    stmt.value->range);
            }
            const auto every_actual_is_covered = std::ranges::all_of(
                actual_places, [&](const ast::ResolvedPlace& actual_place) {
                    return std::ranges::any_of(
                        expected_places,
                        [&](const ast::ResolvedPlace& expected_place) {
                            return is_same_or_subplace(actual_place,
                                                       expected_place);
                        });
                });
            if (expected_places.empty() || !every_actual_is_covered) {
                if (actual_binding.path.empty() &&
                    return_dependency_count == 1) {
                    const auto return_dependency_it = std::ranges::find_if(
                        state.function->return_dependencies,
                        [](const ast::ReturnDependency& dependency) {
                            return dependency.target.is_return &&
                                   dependency.target.resolved_path.empty();
                        });
                    if (return_dependency_it !=
                        state.function->return_dependencies.end()) {
                        const auto& source = return_dependency_it->source;
                        if (is_borrow_like_type(state.return_type)) {
                            return make_error("returned borrow does not match "
                                              "depends(return on " +
                                                  source.root_name + ")",
                                              stmt.value->range);
                        }
                        if (types.unqualify(state.return_type)->kind ==
                            TypeKind::Slice) {
                            return make_error("returned slice does not match "
                                              "depends(return on " +
                                                  source.root_name + ")",
                                              stmt.value->range);
                        }
                    }
                }
                return make_error("returned borrow or slice does not match its "
                                  "declared depends source",
                                  stmt.value->range);
            }
        }
    }
    auto validated_outputs =
        validateMutableParameterDependencies(state, state.function->range);
    if (!validated_outputs) {
        return std::unexpected(validated_outputs.error());
    }
    stmt.drop_local_ids = collectDropLocalIds(
        state, [](const LocalState& local) { return local.in_scope; });
    state.reachable = false;
    return {};
}

auto SemanticAnalyzer::analyzeDrop(FunctionState& state, ast::DropStmt& stmt)
    -> std::expected<void, Diagnostic> {
    auto expr_type = analyzeExpr(state, *stmt.value);
    if (!expr_type) {
        return std::unexpected(expr_type.error());
    }

    auto place = stmt.value->resolved_place;
    if (!place.has_value()) {
        return make_error("drop() requires a place expression",
                          stmt.value->range);
    }

    const auto is_whole_local_drop =
        !place->is_external && place->fields.empty();
    if (is_whole_local_drop) {
        if (types.isCopy(*expr_type)) {
            return make_error("drop() requires a move type", stmt.value->range);
        }
        const auto local_index = findLocalById(state, place->root_id);
        if (!local_index.has_value()) {
            return make_error("drop() requires a local place",
                              stmt.value->range);
        }

        auto writable = ensureCanWrite(state, *place, stmt.value->range,
                                       place->owner_local_id);
        if (!writable) {
            return std::unexpected(writable.error());
        }

        auto& local = state.locals[*local_index];
        releaseReborrowParent(state, local);
        local.status = LocalState::Status::Moved;
        markAggregateViewSlots(state, *place, *expr_type,
                               LocalState::Status::Moved);
        return {};
    }

    if (state.unchecked_depth == 0) {
        return make_error("in-place drop requires an unchecked block",
                          stmt.value->range);
    }
    if (!place->is_external) {
        return make_error("in-place drop requires an external place",
                          stmt.value->range);
    }

    auto write_targets = projectedPlaceTargets(state, *stmt.value);
    if (!write_targets) {
        return std::unexpected(write_targets.error());
    }
    if (write_targets->empty()) {
        *write_targets = std::vector<ast::ResolvedPlace>{*place};
    }
    auto write_ignored_local = [&](const ast::ResolvedPlace& write_target)
        -> std::optional<std::size_t> {
        if (place->owner_local_id.has_value()) {
            return place->owner_local_id;
        }
        if (write_target.owner_local_id.has_value()) {
            return write_target.owner_local_id;
        }
        if (!place->is_external && !place->fields.empty()) {
            const auto owner_index = findLocalById(state, place->root_id);
            if (owner_index.has_value()) {
                const auto& owner_local = state.locals[*owner_index];
                if (is_borrow_like_type(owner_local.type) &&
                    owner_local.type->is_mut) {
                    return owner_local.unique_id;
                }
            }
        }
        return std::nullopt;
    };
    for (const auto& write_target : *write_targets) {
        auto writable = ensureCanWrite(state, write_target, stmt.value->range,
                                       write_ignored_local(write_target));
        if (!writable) {
            return std::unexpected(writable.error());
        }
    }

    if (!types.needsDrop(*expr_type)) {
        return {};
    }
    return {};
}

auto SemanticAnalyzer::analyzeIf(FunctionState& state, ast::IfStmt& stmt)
    -> std::expected<void, Diagnostic> {
    std::vector<std::size_t> original_continue_counts;
    original_continue_counts.reserve(state.loops.size());
    for (const auto& loop : state.loops) {
        original_continue_counts.push_back(loop.continue_states.size());
    }

    auto discard_existing_continue_states = [&](FunctionState& branch_state) {
        for (std::size_t loop_index = 0;
             loop_index < branch_state.loops.size() &&
             loop_index < original_continue_counts.size();
             ++loop_index) {
            auto& continue_states =
                branch_state.loops[loop_index].continue_states;
            const auto shared_count = std::min(
                original_continue_counts[loop_index], continue_states.size());
            continue_states.erase(
                continue_states.begin(),
                std::ranges::next(continue_states.begin(),
                                  static_cast<std::ptrdiff_t>(shared_count)));
        }
    };

    auto condition_type = requireReadable(state, *stmt.condition);
    if (!condition_type) {
        return std::unexpected(condition_type.error());
    }
    if (types.unqualify(*condition_type) != types.boolType()) {
        return make_error("if condition must have type bool",
                          stmt.condition->range);
    }
    clearStatementTemporaries(state);

    auto then_state = state;
    auto analyzed_then = analyzeBlock(then_state, *stmt.then_block);
    if (!analyzed_then) {
        return std::unexpected(analyzed_then.error());
    }

    auto else_state = state;
    if (stmt.else_block != nullptr) {
        auto analyzed_else = analyzeBlock(else_state, *stmt.else_block);
        if (!analyzed_else) {
            return std::unexpected(analyzed_else.error());
        }
    }

    discard_existing_continue_states(then_state);
    discard_existing_continue_states(else_state);
    appendContinueStates(state, then_state);
    appendContinueStates(state, else_state);

    if (then_state.reachable && else_state.reachable) {
        auto merged = branchMerge(state, then_state, else_state,
                                  stmt.condition->range, MergePolicy::Join);
        if (!merged) {
            return std::unexpected(merged.error());
        }
        state.reachable = true;
        return {};
    }

    if (then_state.reachable) {
        state.locals = then_state.locals;
        state.view_slot_locals = then_state.view_slot_locals;
        state.reachable = true;
        return {};
    }
    if (else_state.reachable) {
        state.locals = else_state.locals;
        state.view_slot_locals = else_state.view_slot_locals;
        state.reachable = true;
        return {};
    }
    state.reachable = false;
    return {};
}

auto SemanticAnalyzer::analyzeWhile(FunctionState& state, ast::WhileStmt& stmt)
    -> std::expected<void, Diagnostic> {
    auto condition_type = requireReadable(state, *stmt.condition);
    if (!condition_type) {
        return std::unexpected(condition_type.error());
    }
    if (types.unqualify(*condition_type) != types.boolType()) {
        return make_error("while condition must have type bool",
                          stmt.condition->range);
    }
    clearStatementTemporaries(state);

    const auto loop_range = stmt.condition->range;
    const auto loop_entry_state = state;
    auto body_state = state;
    body_state.loops.push_back(
        LoopState{.entry_locals = state.locals,
                  .body_scope_depth = state.scopes.size(),
                  .continue_states = {}});
    auto analyzed_body = analyzeBlock(body_state, *stmt.body);
    if (!analyzed_body) {
        return std::unexpected(analyzed_body.error());
    }

    const auto loop_state = body_state.loops.back();
    body_state.loops.pop_back();
    if (body_state.reachable) {
        auto merged = branchMerge(body_state, body_state, loop_entry_state,
                                  loop_range, MergePolicy::Exact);
        if (!merged) {
            return std::unexpected(Diagnostic(
                "loop body must preserve move/borrow state across iterations",
                loop_range));
        }
    }

    for (const auto& continue_state : loop_state.continue_states) {
        auto merged_state = loop_entry_state;
        merged_state.locals = continue_state.locals;
        merged_state.scopes = continue_state.scopes;
        merged_state.view_slot_locals = continue_state.view_slot_locals;
        auto merged = branchMerge(merged_state, merged_state, loop_entry_state,
                                  loop_range, MergePolicy::Exact);
        if (!merged) {
            return std::unexpected(Diagnostic(
                "continue is only allowed when loop entry move/borrow state "
                "is restored",
                loop_range));
        }
    }

    state.locals = loop_state.entry_locals;
    state.view_slot_locals = loop_entry_state.view_slot_locals;
    state.reachable = true;
    return {};
}

auto SemanticAnalyzer::analyzeFor(FunctionState& state, ast::ForStmt& stmt)
    -> std::expected<void, Diagnostic> {
    enterScope(state);

    if (stmt.initializer != nullptr) {
        auto analyzed_initializer = analyzeStmt(state, *stmt.initializer);
        if (!analyzed_initializer) {
            return std::unexpected(analyzed_initializer.error());
        }
        clearStatementTemporaries(state);
    }

    if (stmt.condition != nullptr) {
        auto condition_type = requireReadable(state, *stmt.condition);
        if (!condition_type) {
            return std::unexpected(condition_type.error());
        }
        if (types.unqualify(*condition_type) != types.boolType()) {
            return make_error("for condition must have type bool",
                              stmt.condition->range);
        }
        clearStatementTemporaries(state);
    }

    const auto loop_range =
        stmt.condition != nullptr ? stmt.condition->range : stmt.body->range;
    const auto loop_entry_state = state;
    auto body_state = state;
    body_state.loops.push_back(
        LoopState{.entry_locals = loop_entry_state.locals,
                  .body_scope_depth = state.scopes.size(),
                  .continue_states = {}});
    auto analyzed_body = analyzeBlock(body_state, *stmt.body);
    if (!analyzed_body) {
        return std::unexpected(analyzed_body.error());
    }

    const auto loop_state = body_state.loops.back();
    body_state.loops.pop_back();
    auto analyze_iteration_predecessor =
        [&](FunctionState predecessor_state,
            SourceRange predecessor_range) -> std::expected<void, Diagnostic> {
        if (stmt.step != nullptr && predecessor_state.reachable) {
            auto analyzed_step = analyzeStmt(predecessor_state, *stmt.step);
            if (!analyzed_step) {
                return std::unexpected(analyzed_step.error());
            }
            clearStatementTemporaries(predecessor_state);
        }

        if (!predecessor_state.reachable) {
            return {};
        }

        auto merged =
            branchMerge(predecessor_state, predecessor_state, loop_entry_state,
                        predecessor_range, MergePolicy::Exact);
        if (!merged) {
            return std::unexpected(Diagnostic(
                "loop body must preserve move/borrow state across iterations",
                predecessor_range));
        }
        return {};
    };

    if (body_state.reachable) {
        auto merged_body =
            analyze_iteration_predecessor(body_state, stmt.body->range);
        if (!merged_body) {
            return std::unexpected(merged_body.error());
        }
    }

    for (const auto& continue_state : loop_state.continue_states) {
        auto continued_state = loop_entry_state;
        continued_state.locals = continue_state.locals;
        continued_state.scopes = continue_state.scopes;
        continued_state.view_slot_locals = continue_state.view_slot_locals;
        continued_state.reachable = true;
        continued_state.temporary_loans.clear();
        continued_state.temporary_suspended_local_ids.clear();
        auto merged_continue =
            analyze_iteration_predecessor(continued_state, loop_range);
        if (!merged_continue) {
            return std::unexpected(merged_continue.error());
        }
    }

    state.locals = loop_entry_state.locals;
    state.view_slot_locals = loop_entry_state.view_slot_locals;
    state.reachable = true;

    const auto header_scope_depth = state.scopes.size() - 1;
    stmt.exit_drop_local_ids =
        collectDropLocalIds(state, [&](const LocalState& local) {
            return local.in_scope && local.scope_depth == header_scope_depth;
        });
    leaveScope(state);
    return {};
}

auto SemanticAnalyzer::analyzeBreak(FunctionState& state, ast::BreakStmt& stmt)
    -> std::expected<void, Diagnostic> {
    const auto break_range =
        state.function != nullptr ? state.function->range : SourceRange{};
    if (state.loops.empty()) {
        return make_error("break must appear inside a loop", break_range);
    }

    const auto loop_depth = state.loops.back().body_scope_depth;
    auto break_state = state;
    while (break_state.scopes.size() > loop_depth) {
        leaveScope(break_state);
    }
    clearStatementTemporaries(break_state);

    const auto result = requireLoopBreakState(break_state, break_range);
    if (!result) {
        return std::unexpected(result.error());
    }

    stmt.drop_local_ids =
        collectDropLocalIds(state, [&](const LocalState& local) {
            return local.in_scope && local.scope_depth >= loop_depth;
        });
    state.reachable = false;
    return {};
}

auto SemanticAnalyzer::analyzeContinue(FunctionState& state,
                                       ast::ContinueStmt& stmt)
    -> std::expected<void, Diagnostic> {
    if (state.loops.empty()) {
        return make_error("continue must appear inside a loop",
                          state.function != nullptr ? state.function->range
                                                    : SourceRange{});
    }

    const auto loop_depth = state.loops.back().body_scope_depth;
    stmt.drop_local_ids =
        collectDropLocalIds(state, [&](const LocalState& local) {
            return local.in_scope && local.scope_depth >= loop_depth;
        });

    auto continue_state = state;
    while (continue_state.scopes.size() > loop_depth) {
        leaveScope(continue_state);
    }
    clearStatementTemporaries(continue_state);
    state.loops.back().continue_states.push_back(ContinueState{
        .locals = continue_state.locals,
        .scopes = continue_state.scopes,
        .view_slot_locals = continue_state.view_slot_locals,
    });
    state.reachable = false;
    return {};
}

auto SemanticAnalyzer::analyzeUnchecked(FunctionState& state,
                                        ast::UncheckedStmt& stmt)
    -> std::expected<void, Diagnostic> {
    ++state.unchecked_depth;
    auto analyzed = analyzeBlock(state, *stmt.body);
    --state.unchecked_depth;
    if (!analyzed) {
        return std::unexpected(analyzed.error());
    }
    return {};
}

auto SemanticAnalyzer::analyzeSwitch(FunctionState& state,
                                     ast::SwitchStmt& stmt)
    -> std::expected<void, Diagnostic> {
    const auto* unary = std::get_if<ast::UnaryExpr>(&stmt.scrutinee->node);
    if (unary == nullptr) {
        return make_error(
            "switch scrutinee must be one of '&x', '&mut x', or 'move x'",
            stmt.scrutinee->range);
    }

    const Type* enum_type = nullptr;
    std::optional<ast::ResolvedPlace> enum_place;
    std::optional<TemporaryLoan> switch_loan;
    std::vector<ViewLeafBinding> moved_enum_bindings;
    switch (unary->op) {
    case ast::UnaryOp::Borrow: {
        stmt.match_kind = ast::MatchKind::Borrow;
        auto borrowed = borrowFromExpr(state, *stmt.scrutinee, false, true);
        if (!borrowed) {
            return std::unexpected(borrowed.error());
        }
        enum_place = *borrowed;
        switch_loan = TemporaryLoan{.place = *borrowed, .is_mut = false};
        switch_loan->place.owner_local_id.reset();
        enum_type = unary->operand->resolved_type;
        break;
    }
    case ast::UnaryOp::BorrowMut: {
        stmt.match_kind = ast::MatchKind::BorrowMut;
        auto borrowed = borrowFromExpr(state, *stmt.scrutinee, true, true);
        if (!borrowed) {
            return std::unexpected(borrowed.error());
        }
        enum_place = *borrowed;
        switch_loan = TemporaryLoan{.place = *borrowed, .is_mut = true};
        enum_type = unary->operand->resolved_type;
        break;
    }
    case ast::UnaryOp::Move: {
        stmt.match_kind = ast::MatchKind::Move;
        auto analyzed_type = analyzeExpr(state, *unary->operand);
        if (!analyzed_type) {
            return std::unexpected(analyzed_type.error());
        }
        enum_type = *analyzed_type;
        if (typeContainsViews(enum_type)) {
            auto bindings = collectExprViewBindings(state, *unary->operand);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            moved_enum_bindings = std::move(*bindings);
        }
        auto consumed = consumeValue(state, *unary->operand, *analyzed_type);
        if (!consumed) {
            return std::unexpected(consumed.error());
        }
        break;
    }
    default:
        return make_error(
            "switch scrutinee must be one of '&x', '&mut x', or 'move x'",
            stmt.scrutinee->range);
    }

    if (enum_type == nullptr || enum_type->kind != TypeKind::Enum) {
        return make_error("switch scrutinee must have enum type",
                          stmt.scrutinee->range);
    }

    stmt.enum_decl = enum_type->enum_decl;
    std::vector<bool> seen_variants(stmt.enum_decl->variants.size(), false);
    bool saw_default = false;
    SourceRange default_range = stmt.scrutinee->range;
    std::vector<FunctionState> reachable_states;
    const auto switch_entry_state = state;
    std::vector<std::size_t> original_continue_counts;
    original_continue_counts.reserve(state.loops.size());
    for (const auto& loop : state.loops) {
        original_continue_counts.push_back(loop.continue_states.size());
    }

    for (auto& switch_case : stmt.cases) {
        if (switch_case.is_default) {
            if (saw_default) {
                return make_error("duplicate default case", switch_case.range);
            }
            saw_default = true;
            default_range = switch_case.range;

            auto case_state = switch_entry_state;
            if (switch_loan.has_value()) {
                case_state.temporary_loans.clear();
                case_state.temporary_loans.push_back(*switch_loan);
            }
            enterScope(case_state);
            const auto& case_switch_loan = switch_loan;
            for (auto& statement : switch_case.body->statements) {
                if (!case_state.reachable) {
                    break;
                }
                auto analyzed = analyzeStmt(case_state, *statement);
                if (!analyzed) {
                    return std::unexpected(analyzed.error());
                }
                clearStatementTemporaries(case_state);
                if (case_switch_loan.has_value()) {
                    case_state.temporary_loans.push_back(*case_switch_loan);
                }
            }
            leaveScope(case_state);

            for (std::size_t loop_index = 0;
                 loop_index < case_state.loops.size() &&
                 loop_index < original_continue_counts.size();
                 ++loop_index) {
                auto& continue_states =
                    case_state.loops[loop_index].continue_states;
                const auto shared_count =
                    std::min(original_continue_counts[loop_index],
                             continue_states.size());
                continue_states.erase(
                    continue_states.begin(),
                    std::ranges::next(
                        continue_states.begin(),
                        static_cast<std::ptrdiff_t>(shared_count)));
            }
            appendContinueStates(state, case_state);

            if (case_state.reachable) {
                reachable_states.push_back(std::move(case_state));
            }
            continue;
        }

        const auto variant_it = std::ranges::find_if(
            stmt.enum_decl->variants, [&](const ast::EnumVariant& variant) {
                return variant.name == switch_case.variant_name;
            });
        if (variant_it == stmt.enum_decl->variants.end()) {
            return make_error("unknown enum variant '" +
                                  switch_case.variant_name + "'",
                              switch_case.range);
        }
        const auto variant_index = static_cast<std::size_t>(
            std::distance(stmt.enum_decl->variants.begin(), variant_it));
        if (seen_variants[variant_index]) {
            return make_error("duplicate switch case for '" +
                                  switch_case.variant_name + "'",
                              switch_case.range);
        }
        seen_variants[variant_index] = true;
        switch_case.variant_index = static_cast<std::uint32_t>(variant_index);
        const auto& variant = *variant_it;

        auto case_state = switch_entry_state;
        if (switch_loan.has_value()) {
            case_state.temporary_loans.clear();
            case_state.temporary_loans.push_back(*switch_loan);
        }
        enterScope(case_state);
        auto case_switch_loan = switch_loan;
        if (switch_case.binding_name.has_value()) {
            if (variant.resolved_type == nullptr) {
                return make_error(
                    "case binding requires a variant with a payload",
                    switch_case.range);
            }

            const auto* binding_type = variant.resolved_type;
            if (stmt.match_kind == ast::MatchKind::Borrow) {
                binding_type = types.getBorrow(binding_type, false);
            } else if (stmt.match_kind == ast::MatchKind::BorrowMut) {
                binding_type = types.getBorrow(binding_type, true);
            }

            auto declared =
                declareLocal(case_state, *switch_case.binding_name,
                             binding_type, false, switch_case.range);
            if (!declared) {
                return std::unexpected(declared.error());
            }

            auto& binding_local = case_state.locals[*declared];
            binding_local.status = LocalState::Status::Live;
            switch_case.binding_local_id = binding_local.unique_id;
            switch_case.binding_type = binding_type;
            if (is_view_like_type(types, binding_type)) {
                binding_local.is_view_slot = true;
                binding_local.slot_root_is_external = false;
                binding_local.slot_root_id = binding_local.unique_id;
                registerViewSlot(case_state, *declared);
            } else if (typeContainsViews(binding_type)) {
                auto slots = declareAggregateViewSlots(
                    case_state, localPlace(binding_local.unique_id),
                    binding_type, false, LocalState::Status::Uninitialized);
                if (!slots) {
                    return std::unexpected(slots.error());
                }
            }

            if (stmt.match_kind != ast::MatchKind::Move) {
                auto payload_place = *enum_place;
                payload_place.fields.push_back(ENUM_PAYLOAD_SENTINEL);
                payload_place.fields.push_back(
                    static_cast<std::uint32_t>(variant_index));
                setTopLevelOrigins(binding_local, {payload_place});
                if (case_switch_loan.has_value()) {
                    if (stmt.match_kind == ast::MatchKind::BorrowMut) {
                        case_switch_loan->place.owner_local_id =
                            binding_local.unique_id;
                    } else {
                        case_switch_loan->place.owner_local_id.reset();
                    }
                    case_state.temporary_loans.clear();
                    case_state.temporary_loans.push_back(*case_switch_loan);
                }
            } else if (typeContainsViews(binding_type)) {
                std::vector<ViewLeafBinding> payload_bindings;
                const std::vector<std::uint32_t> payload_prefix = {
                    ENUM_PAYLOAD_SENTINEL,
                    static_cast<std::uint32_t>(variant_index),
                };
                for (const auto& binding : moved_enum_bindings) {
                    if (!is_path_prefix(payload_prefix, binding.path)) {
                        continue;
                    }
                    auto rebased = binding;
                    rebased.path.erase(
                        rebased.path.begin(),
                        std::ranges::next(rebased.path.begin(),
                                          static_cast<std::ptrdiff_t>(
                                              payload_prefix.size())));
                    payload_bindings.push_back(std::move(rebased));
                }
                auto assigned = setAggregateViewSlots(
                    case_state, localPlace(binding_local.unique_id),
                    binding_type, payload_bindings, switch_case.range);
                if (!assigned) {
                    return std::unexpected(assigned.error());
                }
            }
        } else if (variant.resolved_type == nullptr) {
            switch_case.binding_type = nullptr;
        }

        for (auto& statement : switch_case.body->statements) {
            if (!case_state.reachable) {
                break;
            }
            auto analyzed = analyzeStmt(case_state, *statement);
            if (!analyzed) {
                return std::unexpected(analyzed.error());
            }
            clearStatementTemporaries(case_state);
            if (case_switch_loan.has_value()) {
                case_state.temporary_loans.push_back(*case_switch_loan);
            }
        }
        leaveScope(case_state);

        for (std::size_t loop_index = 0;
             loop_index < case_state.loops.size() &&
             loop_index < original_continue_counts.size();
             ++loop_index) {
            auto& continue_states =
                case_state.loops[loop_index].continue_states;
            const auto shared_count = std::min(
                original_continue_counts[loop_index], continue_states.size());
            continue_states.erase(
                continue_states.begin(),
                std::ranges::next(continue_states.begin(),
                                  static_cast<std::ptrdiff_t>(shared_count)));
        }
        appendContinueStates(state, case_state);

        if (case_state.reachable) {
            reachable_states.push_back(std::move(case_state));
        }
    }

    const auto is_exhaustive =
        saw_default ||
        std::ranges::none_of(seen_variants, [](bool seen) { return !seen; });
    if (!is_exhaustive) {
        return make_error("switch over enum must be exhaustive",
                          stmt.scrutinee->range);
    }
    if (saw_default &&
        std::ranges::none_of(seen_variants, [](bool seen) { return !seen; })) {
        return make_error("default case is unreachable", default_range);
    }

    if (reachable_states.empty()) {
        state.reachable = false;
        return {};
    }

    auto merged =
        mergeReachableStates(state, reachable_states, stmt.scrutinee->range);
    if (!merged) {
        return std::unexpected(merged.error());
    }
    state.reachable = true;
    return {};
}

auto SemanticAnalyzer::analyzeExprStmt(FunctionState& state,
                                       ast::ExprStmt& stmt)
    -> std::expected<void, Diagnostic> {
    auto expr_type = requireReadable(state, *stmt.expr);
    if (!expr_type) {
        return std::unexpected(expr_type.error());
    }
    return {};
}

} // namespace cyan

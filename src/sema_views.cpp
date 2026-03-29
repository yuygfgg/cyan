#include "sema_detail.hpp"

namespace sc {

using namespace detail;

auto SemanticAnalyzer::typeContainsViews(const Type* type) const -> bool {
    if (type == nullptr) {
        return false;
    }
    if (is_view_like_type(types, type)) {
        return true;
    }

    type = types.unqualify(type);
    switch (type->kind) {
    case TypeKind::Struct:
        return std::ranges::any_of(
            type->struct_decl->fields, [&](const ast::StructField& field) {
                return typeContainsViews(field.resolved_type);
            });
    case TypeKind::Enum:
        return std::ranges::any_of(
            type->enum_decl->variants, [&](const ast::EnumVariant& variant) {
                return variant.resolved_type != nullptr &&
                       typeContainsViews(variant.resolved_type);
            });
    default:
        return false;
    }
}

auto SemanticAnalyzer::collectViewLeafInfos(
    const Type* type, std::vector<std::uint32_t> prefix) const
    -> std::vector<ViewLeafInfo> {
    std::vector<ViewLeafInfo> leaves;
    if (type == nullptr) {
        return leaves;
    }
    if (is_view_like_type(types, type)) {
        leaves.push_back(ViewLeafInfo{std::move(prefix), type});
        return leaves;
    }

    type = types.unqualify(type);
    switch (type->kind) {
    case TypeKind::Struct:
        for (std::size_t index = 0; index < type->struct_decl->fields.size();
             ++index) {
            auto next_prefix = prefix;
            next_prefix.push_back(static_cast<std::uint32_t>(index));
            auto field_leaves = collectViewLeafInfos(
                type->struct_decl->fields[index].resolved_type,
                std::move(next_prefix));
            leaves.insert(leaves.end(), field_leaves.begin(),
                          field_leaves.end());
        }
        break;
    case TypeKind::Enum:
        for (std::size_t index = 0; index < type->enum_decl->variants.size();
             ++index) {
            const auto& variant = type->enum_decl->variants[index];
            if (variant.resolved_type == nullptr ||
                !typeContainsViews(variant.resolved_type)) {
                continue;
            }
            auto next_prefix = prefix;
            next_prefix.push_back(ENUM_PAYLOAD_SENTINEL);
            next_prefix.push_back(static_cast<std::uint32_t>(index));
            auto payload_leaves = collectViewLeafInfos(variant.resolved_type,
                                                       std::move(next_prefix));
            leaves.insert(leaves.end(), payload_leaves.begin(),
                          payload_leaves.end());
        }
        break;
    default:
        break;
    }
    return leaves;
}

auto SemanticAnalyzer::viewSlotKey(bool is_external, std::size_t root_id,
                                   const std::vector<std::uint32_t>& path) const
    -> std::string {
    std::ostringstream stream;
    stream << (is_external ? 'e' : 'l') << ':' << root_id;
    for (const auto field : path) {
        stream << ':' << field;
    }
    return stream.str();
}

auto SemanticAnalyzer::registerViewSlot(FunctionState& state,
                                        std::size_t local_index) -> void {
    const auto& local = state.locals[local_index];
    if (!local.is_view_slot) {
        return;
    }
    state.view_slot_locals.emplace(viewSlotKey(local.slot_root_is_external,
                                               local.slot_root_id,
                                               local.slot_path),
                                   local_index);
}

auto SemanticAnalyzer::findViewSlotLocal(
    FunctionState& state, bool is_external, std::size_t root_id,
    const std::vector<std::uint32_t>& path) const
    -> std::optional<std::size_t> {
    if (const auto it = state.view_slot_locals.find(
            viewSlotKey(is_external, root_id, path));
        it != state.view_slot_locals.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto SemanticAnalyzer::declareAggregateViewSlots(
    FunctionState& state, const ast::ResolvedPlace& base_place,
    const Type* type, bool abstract_sources, LocalState::Status status)
    -> std::expected<void, Diagnostic> {
    if (!typeContainsViews(type)) {
        return {};
    }

    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const auto base_place_copy = base_place;
    const auto leaves = collectViewLeafInfos(type);
    for (const auto& leaf : leaves) {
        auto full_path = base_place_copy.fields;
        full_path.insert(full_path.end(), leaf.path.begin(), leaf.path.end());
        if (findViewSlotLocal(state, base_place_copy.is_external,
                              base_place_copy.root_id, full_path)
                .has_value()) {
            continue;
        }

        const auto local_index = declareHiddenLocal(state, leaf.type);
        auto& local = state.locals[local_index];
        local.status = status;
        local.is_view_slot = true;
        local.slot_root_is_external = base_place_copy.is_external;
        local.slot_root_id = base_place_copy.root_id;
        local.slot_path = std::move(full_path);
        if (abstract_sources && status == LocalState::Status::Live) {
            local.borrow_origin = ast::ResolvedPlace{
                .root_id = next_external_root++,
                .is_external = true,
                .owner_local_id = std::nullopt,
                .fields = {},
            };
        }
        registerViewSlot(state, local_index);
    }
    return {};
}

auto SemanticAnalyzer::ensureAggregateViewSlots(
    FunctionState& state, const ast::ResolvedPlace& base_place,
    const Type* type) -> std::expected<void, Diagnostic> {
    if (!typeContainsViews(type)) {
        return {};
    }

    const auto leaves = collectViewLeafInfos(type);
    const auto missing_leaf =
        std::ranges::find_if(leaves, [&](const ViewLeafInfo& leaf) {
            auto full_path = base_place.fields;
            full_path.insert(full_path.end(), leaf.path.begin(),
                             leaf.path.end());
            return !findViewSlotLocal(state, base_place.is_external,
                                      base_place.root_id, full_path)
                        .has_value();
        });
    if (missing_leaf == leaves.end()) {
        return {};
    }

    if (base_place.is_external) {
        return declareAggregateViewSlots(state, base_place, type, true,
                                         LocalState::Status::Live);
    }

    auto local_index = findLocalById(state, base_place.root_id);
    if (!local_index.has_value()) {
        return make_error("invalid aggregate view source", SourceRange{});
    }
    return declareAggregateViewSlots(state, base_place, type, false,
                                     state.locals[*local_index].status);
}

auto SemanticAnalyzer::ensureViewSubtreeLive(
    FunctionState& state, const ast::ResolvedPlace& base_place,
    const Type* type, SourceRange range) -> std::expected<void, Diagnostic> {
    if (!typeContainsViews(type)) {
        return {};
    }
    if (base_place.root_id == 0) {
        return make_error("invalid aggregate view source", range);
    }
    auto ensured = ensureAggregateViewSlots(state, base_place, type);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    for (const auto& leaf : collectViewLeafInfos(type)) {
        auto full_path = base_place.fields;
        full_path.insert(full_path.end(), leaf.path.begin(), leaf.path.end());
        const auto slot_index = findViewSlotLocal(
            state, base_place.is_external, base_place.root_id, full_path);
        if (!slot_index.has_value()) {
            return make_error("aggregate view slot is missing", range);
        }
        const auto& slot = state.locals[*slot_index];
        if (!slot.in_scope || slot.status != LocalState::Status::Live) {
            return make_error("cannot use a moved or uninitialized view value",
                              range);
        }
        auto source_live = ensureViewSourceLive(state, slot, range);
        if (!source_live) {
            return std::unexpected(source_live.error());
        }
    }
    return {};
}

auto SemanticAnalyzer::collectSlotBindings(FunctionState& state,
                                           const ast::ResolvedPlace& base_place,
                                           const Type* type)
    -> std::expected<std::vector<ViewLeafBinding>, Diagnostic> {
    auto live = ensureViewSubtreeLive(state, base_place, type, SourceRange{});
    if (!live) {
        return std::unexpected(live.error());
    }

    std::vector<ViewLeafBinding> bindings;
    for (const auto& leaf : collectViewLeafInfos(type)) {
        auto full_path = base_place.fields;
        full_path.insert(full_path.end(), leaf.path.begin(), leaf.path.end());
        const auto slot_index = findViewSlotLocal(
            state, base_place.is_external, base_place.root_id, full_path);
        if (!slot_index.has_value()) {
            return unexpected_result<std::vector<ViewLeafBinding>>(
                "aggregate view slot is missing", SourceRange{});
        }
        const auto& slot = state.locals[*slot_index];
        bindings.push_back(ViewLeafBinding{
            .path = leaf.path,
            .source_place = slot.borrow_origin,
            .source_local_id = is_borrow_like_type(slot.type)
                                   ? std::optional<std::size_t>(slot.unique_id)
                                   : std::nullopt,
            .element_sources = slot.element_origins,
            .type = leaf.type,
        });
    }
    return bindings;
}

auto SemanticAnalyzer::collectExprViewBindings(FunctionState& state,
                                               ast::Expr& expr)
    -> std::expected<std::vector<ViewLeafBinding>, Diagnostic> {
    if (expr.cached_view_bindings.has_value()) {
        std::vector<ViewLeafBinding> bindings;
        bindings.reserve(expr.cached_view_bindings->size());
        for (const auto& binding : *expr.cached_view_bindings) {
            bindings.push_back(ViewLeafBinding{
                .path = binding.path,
                .source_place = binding.source_place,
                .source_local_id = binding.source_local_id,
                .element_sources = binding.element_sources,
                .type = nullptr,
            });
        }
        return bindings;
    }

    const Type* expr_type = expr.resolved_type;
    if (expr_type == nullptr) {
        auto analyzed = analyzeExpr(state, expr);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        expr_type = *analyzed;
    }
    if (!typeContainsViews(expr_type)) {
        return std::vector<ViewLeafBinding>{};
    }

    if (is_view_like_type(types, expr_type)) {
        ViewLeafBinding binding{
            .path = {},
            .source_place = std::nullopt,
            .source_local_id = std::nullopt,
            .element_sources = {},
            .type = expr_type,
        };
        if (is_borrow_like_type(expr_type)) {
            auto place = borrowSourcePlace(state, expr);
            if (!place) {
                return std::unexpected(place.error());
            }
            auto local_id = borrowSourceLocalId(state, expr);
            if (!local_id) {
                return std::unexpected(local_id.error());
            }
            binding.source_place = *place;
            binding.source_local_id = *local_id;
        } else {
            auto place = sliceSourcePlace(state, expr);
            if (!place) {
                return std::unexpected(place.error());
            }
            binding.source_place = *place;
        }
        if (is_direct_shared_view_slice(types, expr_type) &&
            expr.resolved_place.has_value()) {
            const auto slot_index = findViewSlotLocal(
                state, expr.resolved_place->is_external,
                expr.resolved_place->root_id, expr.resolved_place->fields);
            if (slot_index.has_value()) {
                binding.element_sources =
                    state.locals[*slot_index].element_origins;
            }
        }
        return std::vector<ViewLeafBinding>{std::move(binding)};
    }

    if (expr.resolved_place.has_value()) {
        return collectSlotBindings(state, *expr.resolved_place, expr_type);
    }

    if (auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr && unary->op == ast::UnaryOp::Move) {
        return collectExprViewBindings(state, *unary->operand);
    }

    if (auto* init_list = std::get_if<ast::InitListExpr>(&expr.node);
        init_list != nullptr &&
        types.unqualify(expr_type)->kind == TypeKind::Struct) {
        std::vector<ViewLeafBinding> bindings;
        const auto& fields = types.unqualify(expr_type)->struct_decl->fields;
        for (std::size_t index = 0; index < init_list->elements.size();
             ++index) {
            auto child_bindings =
                collectExprViewBindings(state, *init_list->elements[index]);
            if (!child_bindings) {
                return std::unexpected(child_bindings.error());
            }
            for (auto& binding : *child_bindings) {
                binding.path.insert(binding.path.begin(),
                                    static_cast<std::uint32_t>(index));
                if (binding.type == nullptr) {
                    binding.type = fields[index].resolved_type;
                }
                bindings.push_back(std::move(binding));
            }
        }
        return bindings;
    }

    if (auto* call = std::get_if<ast::CallExpr>(&expr.node); call != nullptr) {
        if (call->enum_decl != nullptr) {
            const auto& variant =
                call->enum_decl->variants[call->variant_index];
            if (variant.resolved_type == nullptr ||
                !typeContainsViews(variant.resolved_type)) {
                return std::vector<ViewLeafBinding>{};
            }
            auto payload_bindings =
                collectExprViewBindings(state, *call->arguments.front());
            if (!payload_bindings) {
                return std::unexpected(payload_bindings.error());
            }
            for (auto& binding : *payload_bindings) {
                binding.path.insert(binding.path.begin(), call->variant_index);
                binding.path.insert(binding.path.begin(),
                                    ENUM_PAYLOAD_SENTINEL);
            }
            return payload_bindings;
        }

        if (call->function != nullptr &&
            !call->function->return_dependencies.empty()) {
            std::vector<ViewLeafBinding> bindings;
            std::vector<std::vector<ViewLeafBinding>> cached_arguments(
                call->arguments.size());
            std::vector<bool> argument_ready(call->arguments.size(), false);
            auto find_binding =
                [](std::vector<ViewLeafBinding>& candidates,
                   const std::vector<std::uint32_t>& path) -> ViewLeafBinding* {
                const auto it = std::ranges::find_if(
                    candidates, [&](const ViewLeafBinding& binding) {
                        return binding.path == path;
                    });
                return it == candidates.end() ? nullptr : &(*it);
            };

            for (const auto& dependency : call->function->return_dependencies) {
                if (!dependency.source.parameter_index.has_value()) {
                    continue;
                }
                const auto parameter_index = *dependency.source.parameter_index;
                if (!argument_ready[parameter_index]) {
                    auto argument_bindings = collectExprViewBindings(
                        state, *call->arguments[parameter_index]);
                    if (!argument_bindings) {
                        return std::unexpected(argument_bindings.error());
                    }
                    cached_arguments[parameter_index] =
                        std::move(*argument_bindings);
                    argument_ready[parameter_index] = true;
                }

                const auto target_leaves =
                    collectViewLeafInfos(dependency.target.resolved_type);
                const auto source_leaves =
                    collectViewLeafInfos(dependency.source.resolved_type);
                for (std::size_t index = 0; index < target_leaves.size();
                     ++index) {
                    auto source_path = dependency.source.resolved_path;
                    source_path.insert(source_path.end(),
                                       source_leaves[index].path.begin(),
                                       source_leaves[index].path.end());
                    auto* source_binding = find_binding(
                        cached_arguments[parameter_index], source_path);
                    if (source_binding == nullptr) {
                        return unexpected_result<std::vector<ViewLeafBinding>>(
                            "call dependency source is missing a tracked "
                            "borrow or slice leaf",
                            expr.range);
                    }

                    auto target_path = dependency.target.resolved_path;
                    target_path.insert(target_path.end(),
                                       target_leaves[index].path.begin(),
                                       target_leaves[index].path.end());
                    bindings.push_back(ViewLeafBinding{
                        .path = std::move(target_path),
                        .source_place = source_binding->source_place,
                        .source_local_id = source_binding->source_local_id,
                        .element_sources = source_binding->element_sources,
                        .type = target_leaves[index].type,
                    });
                }
            }
            return bindings;
        }
    }

    return unexpected_result<std::vector<ViewLeafBinding>>(
        "could not determine aggregate borrow or slice sources", expr.range);
}

auto SemanticAnalyzer::setAggregateViewSlots(
    FunctionState& state, const ast::ResolvedPlace& target_place,
    const Type* target_type, const std::vector<ViewLeafBinding>& bindings,
    SourceRange range) -> std::expected<void, Diagnostic> {
    if (!typeContainsViews(target_type)) {
        return {};
    }
    if (target_place.root_id == 0) {
        return make_error("invalid aggregate assignment target", SourceRange{});
    }

    auto ensured = ensureAggregateViewSlots(state, target_place, target_type);
    if (!ensured) {
        return std::unexpected(ensured.error());
    }

    auto find_binding =
        [&](const std::vector<std::uint32_t>& path) -> const ViewLeafBinding* {
        const auto it =
            std::ranges::find_if(bindings, [&](const ViewLeafBinding& binding) {
                return binding.path == path;
            });
        return it == bindings.end() ? nullptr : &(*it);
    };

    for (const auto& leaf : collectViewLeafInfos(target_type)) {
        const auto* binding = find_binding(leaf.path);
        if (binding == nullptr) {
            return make_error(
                "missing borrow or slice source for aggregate assignment",
                SourceRange{});
        }

        auto full_path = target_place.fields;
        full_path.insert(full_path.end(), leaf.path.begin(), leaf.path.end());
        const auto slot_index = findViewSlotLocal(
            state, target_place.is_external, target_place.root_id, full_path);
        if (!slot_index.has_value()) {
            return make_error(
                "aggregate assignment target is missing a view slot",
                SourceRange{});
        }

        auto& slot = state.locals[*slot_index];
        releaseReborrowParent(state, slot);
        slot.borrow_origin = binding->source_place;
        slot.element_origins = binding->element_sources;
        auto outlives =
            ensureViewSourceOutlivesLocal(state, slot, std::nullopt, range);
        if (!outlives) {
            return std::unexpected(outlives.error());
        }
        slot.status = LocalState::Status::Live;
        if (is_borrow_like_type(slot.type) && slot.type->is_mut &&
            binding->source_local_id.has_value() &&
            *binding->source_local_id != slot.unique_id) {
            const auto parent_index =
                findLocalById(state, *binding->source_local_id);
            if (parent_index.has_value()) {
                auto& parent = state.locals[*parent_index];
                if (is_borrow_like_type(parent.type) && parent.type->is_mut) {
                    slot.reborrow_parent_local_id = parent.unique_id;
                    parent.status = LocalState::Status::Moved;
                }
            }
        }
    }
    return {};
}

auto SemanticAnalyzer::markAggregateViewSlots(
    FunctionState& state, const ast::ResolvedPlace& base_place,
    const Type* type, LocalState::Status status) -> void {
    if (!typeContainsViews(type)) {
        return;
    }
    for (const auto& leaf : collectViewLeafInfos(type)) {
        auto full_path = base_place.fields;
        full_path.insert(full_path.end(), leaf.path.begin(), leaf.path.end());
        const auto slot_index = findViewSlotLocal(
            state, base_place.is_external, base_place.root_id, full_path);
        if (!slot_index.has_value()) {
            continue;
        }
        auto& slot = state.locals[*slot_index];
        if (status != LocalState::Status::Live) {
            releaseReborrowParent(state, slot);
        }
        slot.status = status;
    }
}

auto SemanticAnalyzer::resolveDependencyPath(
    ast::DependencyPath& path, const Type* root_type,
    std::optional<std::size_t> parameter_index)
    -> std::expected<void, Diagnostic> {
    path.parameter_index = parameter_index;
    path.resolved_path.clear();

    const Type* current_type = root_type;
    for (const auto& segment : path.segments) {
        if (current_type == nullptr) {
            return make_error("invalid depends path", segment.range);
        }
        const auto* current_base = types.unqualify(current_type);
        if (current_base->kind == TypeKind::Struct &&
            current_base->struct_decl != nullptr) {
            const auto& fields = current_base->struct_decl->fields;
            const auto field_it = std::ranges::find_if(
                fields, [&](const ast::StructField& field) {
                    return field.name == segment.name;
                });
            if (field_it == fields.end()) {
                return make_error("unknown depends field '" + segment.name +
                                      "'",
                                  segment.range);
            }
            path.resolved_path.push_back(static_cast<std::uint32_t>(
                std::distance(fields.begin(), field_it)));
            current_type = field_it->resolved_type;
            continue;
        }

        if (current_base->kind == TypeKind::Enum &&
            current_base->enum_decl != nullptr) {
            const auto& variants = current_base->enum_decl->variants;
            const auto variant_it = std::ranges::find_if(
                variants, [&](const ast::EnumVariant& variant) {
                    return variant.name == segment.name;
                });
            if (variant_it == variants.end()) {
                return make_error("unknown depends variant '" + segment.name +
                                      "'",
                                  segment.range);
            }
            if (variant_it->resolved_type == nullptr) {
                return make_error(
                    "depends variant path requires a payload-bearing variant",
                    segment.range);
            }
            path.resolved_path.push_back(ENUM_PAYLOAD_SENTINEL);
            path.resolved_path.push_back(static_cast<std::uint32_t>(
                std::distance(variants.begin(), variant_it)));
            current_type = variant_it->resolved_type;
            continue;
        }

        return make_error("depends path can only traverse struct fields or "
                          "enum payload variants",
                          segment.range);
    }

    if (!typeContainsViews(current_type)) {
        if (!path.is_return && path.segments.empty()) {
            return make_error(
                "depends target must be a borrow or slice parameter",
                path.range);
        }
        return make_error("depends paths must resolve to a borrow, slice, or "
                          "aggregate containing them",
                          path.range);
    }
    path.resolved_type = current_type;
    return {};
}

auto SemanticAnalyzer::validateReturnDependencies(ast::FunctionDecl& decl)
    -> std::expected<void, Diagnostic> {
    const auto returns_views = typeContainsViews(decl.resolved_return_type);
    if (!decl.return_dependencies.empty() && !returns_views) {
        return make_error("depends clause is only valid on functions whose "
                          "return type contains a borrow or slice",
                          decl.range);
    }
    if (!returns_views) {
        return {};
    }

    const auto return_leaves = collectViewLeafInfos(decl.resolved_return_type);
    if (std::ranges::any_of(return_leaves, [&](const ViewLeafInfo& leaf) {
            return is_direct_shared_view_slice(types, leaf.type);
        })) {
        return make_error("functions cannot return slices whose elements are "
                          "shared borrow or interface values",
                          decl.range);
    }

    if (decl.return_dependencies.empty()) {
        if (is_view_like_type(types, decl.resolved_return_type)) {
            std::optional<std::size_t> only_view_parameter;
            for (std::size_t index = 0; index < decl.parameters.size();
                 ++index) {
                if (!is_view_like_type(types,
                                       decl.parameters[index].resolved_type)) {
                    continue;
                }
                if (only_view_parameter.has_value()) {
                    return make_error(
                        "view-returning functions with multiple borrow or "
                        "slice parameters require depends(return on <param>)",
                        decl.range);
                }
                only_view_parameter = index;
            }
            if (!only_view_parameter.has_value()) {
                return make_error("view-returning function must depend on a "
                                  "borrow or slice parameter",
                                  decl.range);
            }

            ast::ReturnDependency dependency;
            dependency.target.is_return = true;
            dependency.target.range = decl.return_type->range;
            dependency.source.is_return = false;
            dependency.source.root_name =
                decl.parameters[*only_view_parameter].name;
            dependency.source.root_range =
                decl.parameters[*only_view_parameter].name_range;
            dependency.source.range =
                decl.parameters[*only_view_parameter].name_range;
            dependency.range = decl.range;
            decl.return_dependencies.push_back(std::move(dependency));
        } else {
            return make_error("functions returning aggregates with borrow or "
                              "slice fields require an explicit depends clause",
                              decl.range);
        }
    }

    std::vector<std::vector<std::uint32_t>> covered_leaves;
    covered_leaves.reserve(return_leaves.size());

    for (auto& dependency : decl.return_dependencies) {
        if (!dependency.target.is_return) {
            return make_error("depends target must start with 'return'",
                              dependency.target.range);
        }
        auto resolved_target = resolveDependencyPath(
            dependency.target, decl.resolved_return_type, std::nullopt);
        if (!resolved_target) {
            return std::unexpected(resolved_target.error());
        }

        std::optional<std::size_t> parameter_index;
        const Type* source_root_type = nullptr;
        for (std::size_t index = 0; index < decl.parameters.size(); ++index) {
            if (decl.parameters[index].name != dependency.source.root_name) {
                continue;
            }
            parameter_index = index;
            source_root_type = decl.parameters[index].resolved_type;
            break;
        }
        if (!parameter_index.has_value() || source_root_type == nullptr) {
            return make_error("depends source '" + dependency.source.root_name +
                                  "' is not a parameter",
                              dependency.source.range);
        }

        auto resolved_source = resolveDependencyPath(
            dependency.source, source_root_type, parameter_index);
        if (!resolved_source) {
            return std::unexpected(resolved_source.error());
        }

        const auto target_suffixes =
            collectViewLeafInfos(dependency.target.resolved_type);
        const auto source_suffixes =
            collectViewLeafInfos(dependency.source.resolved_type);
        if (target_suffixes.size() != source_suffixes.size()) {
            return make_error("depends source and target must cover the same "
                              "number of borrow or slice leaves",
                              dependency.range);
        }
        for (std::size_t index = 0; index < target_suffixes.size(); ++index) {
            if (target_suffixes[index].path != source_suffixes[index].path) {
                return make_error("depends source and target must have "
                                  "matching aggregate view structure",
                                  dependency.range);
            }
            auto full_target_path = dependency.target.resolved_path;
            full_target_path.insert(full_target_path.end(),
                                    target_suffixes[index].path.begin(),
                                    target_suffixes[index].path.end());
            if (std::ranges::find(covered_leaves, full_target_path) !=
                covered_leaves.end()) {
                return make_error(
                    "depends clause overlaps on the same return path",
                    dependency.range);
            }
            covered_leaves.push_back(std::move(full_target_path));
        }
    }

    for (const auto& leaf : return_leaves) {
        if (std::ranges::find(covered_leaves, leaf.path) ==
            covered_leaves.end()) {
            return make_error("depends clause must cover every borrow and "
                              "slice path in the return type",
                              decl.range);
        }
    }

    return {};
}

} // namespace sc

#include "sema_detail.hpp"

namespace cyan {

using namespace detail;

namespace {

auto append_projected_path(std::vector<ast::ResolvedPlace> sources,
                           const std::vector<std::uint32_t>& suffix)
    -> std::vector<ast::ResolvedPlace> {
    for (auto& source : sources) {
        source.fields.insert(source.fields.end(), suffix.begin(), suffix.end());
    }
    return sources;
}

auto append_unique_place(std::vector<ast::ResolvedPlace>& places,
                         const ast::ResolvedPlace& place) -> void {
    if (std::ranges::find(places, place) == places.end()) {
        places.push_back(place);
    }
}

} // namespace

auto SemanticAnalyzer::typeContainsViews(const Type* type) const -> bool {
    if (type == nullptr) {
        return false;
    }
    if (is_view_like_type(types, type)) {
        return true;
    }

    type = types.unqualify(type);
    switch (type->kind) {
    case TypeKind::Array:
        return typeContainsViews(type->element_type);
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

auto SemanticAnalyzer::viewShape(const Type* type) const -> const ViewShape& {
    static const ViewShape empty_shape{};
    if (type == nullptr) {
        return empty_shape;
    }

    const auto* shape_key = types.unqualify(type);
    if (const auto it = view_shapes.find(shape_key); it != view_shapes.end()) {
        return it->second;
    }

    ViewShape shape;
    shape.type = shape_key;
    std::function<std::vector<ViewLeafInfo>(const Type*,
                                            std::vector<std::uint32_t>)>
        collect =
            [&](const Type* current_type, std::vector<std::uint32_t> prefix)
        -> std::vector<ViewLeafInfo> {
        std::vector<ViewLeafInfo> leaves;
        if (current_type == nullptr) {
            return leaves;
        }
        if (is_view_like_type(types, current_type)) {
            leaves.push_back(ViewLeafInfo{std::move(prefix), current_type});
            return leaves;
        }

        current_type = types.unqualify(current_type);
        switch (current_type->kind) {
        case TypeKind::Array: {
            if (current_type->array_size == 0) {
                break;
            }
            auto next_prefix = prefix;
            next_prefix.push_back(INDEX_FIELD_SENTINEL);
            auto element_leaves =
                collect(current_type->element_type, std::move(next_prefix));
            leaves.insert(leaves.end(),
                          std::make_move_iterator(element_leaves.begin()),
                          std::make_move_iterator(element_leaves.end()));
            break;
        }
        case TypeKind::Struct:
            for (std::size_t index = 0;
                 index < current_type->struct_decl->fields.size(); ++index) {
                auto next_prefix = prefix;
                next_prefix.push_back(static_cast<std::uint32_t>(index));
                auto field_leaves = collect(
                    current_type->struct_decl->fields[index].resolved_type,
                    std::move(next_prefix));
                leaves.insert(leaves.end(),
                              std::make_move_iterator(field_leaves.begin()),
                              std::make_move_iterator(field_leaves.end()));
            }
            break;
        case TypeKind::Enum:
            for (std::size_t index = 0;
                 index < current_type->enum_decl->variants.size(); ++index) {
                const auto& variant = current_type->enum_decl->variants[index];
                if (variant.resolved_type == nullptr) {
                    continue;
                }
                auto payload_leaves = collect(variant.resolved_type, {});
                if (payload_leaves.empty()) {
                    continue;
                }
                for (auto& leaf : payload_leaves) {
                    leaf.path.insert(leaf.path.begin(),
                                     static_cast<std::uint32_t>(index));
                    leaf.path.insert(leaf.path.begin(), ENUM_PAYLOAD_SENTINEL);
                    leaf.path.insert(leaf.path.begin(), prefix.begin(),
                                     prefix.end());
                }
                leaves.insert(leaves.end(),
                              std::make_move_iterator(payload_leaves.begin()),
                              std::make_move_iterator(payload_leaves.end()));
            }
            break;
        default:
            break;
        }
        return leaves;
    };
    shape.leaves = collect(shape_key, {});
    const auto [inserted_it, _] =
        view_shapes.emplace(shape_key, std::move(shape));
    return inserted_it->second;
}

auto SemanticAnalyzer::collectViewLeafInfos(const Type* type) const
    -> const std::vector<ViewLeafInfo>& {
    return viewShape(type).leaves;
}

auto SemanticAnalyzer::findViewBinding(
    std::vector<ViewLeafBinding>& bindings,
    const std::vector<std::uint32_t>& path) const -> ViewLeafBinding* {
    const auto it =
        std::ranges::find_if(bindings, [&](const ViewLeafBinding& binding) {
            return binding.path == path;
        });
    return it == bindings.end() ? nullptr : &(*it);
}

auto SemanticAnalyzer::findViewBinding(
    const std::vector<ViewLeafBinding>& bindings,
    const std::vector<std::uint32_t>& path) const -> const ViewLeafBinding* {
    const auto it =
        std::ranges::find_if(bindings, [&](const ViewLeafBinding& binding) {
            return binding.path == path;
        });
    return it == bindings.end() ? nullptr : &(*it);
}

auto SemanticAnalyzer::bindingPlaces(const ViewLeafBinding& binding) const
    -> const std::vector<ast::ResolvedPlace>* {
    if (binding.type != nullptr &&
        is_direct_shared_view_slice(types, binding.type) &&
        !binding.element_sources.empty()) {
        return &binding.element_sources;
    }
    if (!binding.source_places.empty()) {
        return &binding.source_places;
    }
    return nullptr;
}

auto SemanticAnalyzer::extendBindingsWithProjectedPointee(
    FunctionState& state, std::vector<ViewLeafBinding>& bindings,
    const Type* parameter_type, bool clear_mut_source_locals, SourceRange range)
    -> std::expected<void, Diagnostic> {
    if (parameter_type == nullptr || parameter_type->kind != TypeKind::Borrow ||
        parameter_type->element_type == nullptr) {
        return {};
    }

    auto* top_level_binding = findViewBinding(bindings, {});
    if (top_level_binding == nullptr) {
        return unexpected_result<void>(
            "dependency source is missing a tracked borrow or slice leaf",
            range);
    }
    auto projected_bindings = collectProjectedViewBindings(
        state, top_level_binding->source_places, parameter_type->element_type);
    if (!projected_bindings) {
        return std::unexpected(projected_bindings.error());
    }
    if (clear_mut_source_locals && parameter_type->is_mut) {
        for (auto& binding : *projected_bindings) {
            binding.source_local_id.reset();
        }
    }
    bindings.insert(bindings.end(),
                    std::make_move_iterator(projected_bindings->begin()),
                    std::make_move_iterator(projected_bindings->end()));
    return {};
}

auto SemanticAnalyzer::buildLeafDependencyBinding(
    const ast::ReturnDependency& dependency,
    const std::vector<ViewLeafBinding>& source_bindings,
    bool include_target_prefix, SourceRange range) const
    -> std::expected<ViewLeafBinding, Diagnostic> {
    const auto* source_binding =
        findViewBinding(source_bindings, dependency.source.resolved_path);
    if (source_binding == nullptr) {
        return unexpected_result<ViewLeafBinding>(
            "dependency source is missing a tracked borrow or slice leaf",
            range);
    }

    return ViewLeafBinding{
        .path = include_target_prefix ? dependency.target.resolved_path
                                      : std::vector<std::uint32_t>{},
        .source_places = source_binding->source_places,
        .source_local_id = source_binding->source_local_id,
        .element_sources = source_binding->element_sources,
        .type = dependency.target.resolved_type,
    };
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
            setTopLevelOrigins(local, {ast::ResolvedPlace{
                                          .root_id = next_external_root++,
                                          .is_external = true,
                                          .owner_local_id = std::nullopt,
                                          .fields = {},
                                      }});
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
        if (!slot.in_scope || !isDefinitelyLive(slot.status)) {
            return make_error(viewStatusMessage(slot.status), range);
        }
        auto source_live = ensureViewSourceLive(state, slot, range);
        if (!source_live) {
            return std::unexpected(source_live.error());
        }
    }
    return {};
}

auto SemanticAnalyzer::viewStatusMessage(LocalState::Status status) const
    -> std::string {
    switch (status) {
    case LocalState::Status::Moved:
        return "cannot use a moved view value";
    case LocalState::Status::Uninitialized:
        return "cannot use an uninitialized view value";
    case LocalState::Status::MaybeLive:
        return "view value may be moved or uninitialized due to control flow";
    case LocalState::Status::Unavailable:
        return "cannot use a view value whose source is unknown after this "
               "call or control-flow path";
    case LocalState::Status::Live:
        break;
    }
    return "cannot use an invalid view value";
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
        const auto source_places = topLevelOrigins(slot);
        bindings.push_back(ViewLeafBinding{
            .path = leaf.path,
            .source_places = source_places,
            .source_local_id = is_borrow_like_type(slot.type)
                                   ? std::optional<std::size_t>(slot.unique_id)
                                   : std::nullopt,
            .element_sources = slot.element_origins,
            .type = leaf.type,
        });
    }
    return bindings;
}

auto SemanticAnalyzer::collectProjectedViewBindings(
    FunctionState& state, const std::vector<ast::ResolvedPlace>& base_places,
    const Type* type)
    -> std::expected<std::vector<ViewLeafBinding>, Diagnostic> {
    std::vector<ViewLeafBinding> bindings;
    auto merge_binding = [&](ViewLeafBinding next_binding) -> void {
        const auto existing_it =
            std::ranges::find_if(bindings, [&](const ViewLeafBinding& binding) {
                return binding.path == next_binding.path;
            });
        if (existing_it == bindings.end()) {
            bindings.push_back(std::move(next_binding));
            return;
        }

        for (const auto& place : next_binding.source_places) {
            append_unique_place(existing_it->source_places, place);
        }
        for (const auto& place : next_binding.element_sources) {
            append_unique_place(existing_it->element_sources, place);
        }
        if (!existing_it->source_local_id.has_value()) {
            existing_it->source_local_id = next_binding.source_local_id;
        } else if (next_binding.source_local_id.has_value() &&
                   existing_it->source_local_id !=
                       next_binding.source_local_id) {
            existing_it->source_local_id.reset();
        }
    };

    for (const auto& base_place : base_places) {
        auto projected_bindings = collectSlotBindings(state, base_place, type);
        if (!projected_bindings) {
            return std::unexpected(projected_bindings.error());
        }
        for (auto& binding : *projected_bindings) {
            merge_binding(std::move(binding));
        }
    }
    return bindings;
}

auto SemanticAnalyzer::projectedPlaceSources(FunctionState& state,
                                             ast::Expr& expr)
    -> std::expected<std::vector<ast::ResolvedPlace>, Diagnostic> {
    const Type* expr_type = expr.resolved_type;
    if (expr_type == nullptr) {
        auto analyzed = analyzeExpr(state, expr);
        if (!analyzed) {
            return std::unexpected(analyzed.error());
        }
        expr_type = *analyzed;
    }

    if (is_borrow_like_type(expr_type) &&
        expr.cached_view_bindings.has_value()) {
        const auto it = std::ranges::find_if(
            *expr.cached_view_bindings,
            [](const ast::CachedViewBinding& binding) {
                return binding.path.empty() && !binding.element_sources.empty();
            });
        if (it != expr.cached_view_bindings->end()) {
            return it->element_sources;
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
                    !local.element_origins.empty()) {
                    return local.element_origins;
                }
            }
        }
        return std::vector<ast::ResolvedPlace>{};
    }

    if (expr.resolved_place.has_value() && is_borrow_like_type(expr_type)) {
        if (const auto slot_index = findViewSlotLocal(
                state, expr.resolved_place->is_external,
                expr.resolved_place->root_id, expr.resolved_place->fields);
            slot_index.has_value()) {
            const auto& slot = state.locals[*slot_index];
            if (!slot.element_origins.empty()) {
                return slot.element_origins;
            }
        }
    }

    if (auto* unary = std::get_if<ast::UnaryExpr>(&expr.node);
        unary != nullptr) {
        switch (unary->op) {
        case ast::UnaryOp::Move:
        case ast::UnaryOp::Borrow:
        case ast::UnaryOp::BorrowMut:
        case ast::UnaryOp::Dereference:
            return projectedPlaceSources(state, *unary->operand);
        default:
            break;
        }
        return std::vector<ast::ResolvedPlace>{};
    }

    if (auto* member = std::get_if<ast::MemberExpr>(&expr.node);
        member != nullptr) {
        auto base_sources = projectedPlaceSources(state, *member->base);
        if (!base_sources) {
            return std::unexpected(base_sources.error());
        }
        if (base_sources->empty()) {
            return std::vector<ast::ResolvedPlace>{};
        }
        std::vector<std::uint32_t> suffix = {member->field_index};
        return append_projected_path(std::move(*base_sources), suffix);
    }

    if (auto* index = std::get_if<ast::IndexExpr>(&expr.node);
        index != nullptr) {
        auto base_sources = projectedPlaceSources(state, *index->base);
        if (!base_sources) {
            return std::unexpected(base_sources.error());
        }
        if (base_sources->empty()) {
            return std::vector<ast::ResolvedPlace>{};
        }
        std::vector<std::uint32_t> suffix = {INDEX_FIELD_SENTINEL};
        return append_projected_path(std::move(*base_sources), suffix);
    }

    return std::vector<ast::ResolvedPlace>{};
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
                .source_places = binding.source_places,
                .source_local_id = binding.source_local_id,
                .element_sources = binding.element_sources,
                .type = binding.type,
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
            .source_places = {},
            .source_local_id = std::nullopt,
            .element_sources = {},
            .type = expr_type,
        };
        if (is_borrow_like_type(expr_type)) {
            auto local_id = borrowSourceLocalId(state, expr);
            if (!local_id) {
                return std::unexpected(local_id.error());
            }
            binding.source_local_id = *local_id;
            if (!binding.source_local_id.has_value() &&
                expr.resolved_place.has_value()) {
                const auto slot_index = findViewSlotLocal(
                    state, expr.resolved_place->is_external,
                    expr.resolved_place->root_id, expr.resolved_place->fields);
                if (slot_index.has_value()) {
                    binding.source_local_id =
                        state.locals[*slot_index].unique_id;
                }
            }
            if (expr.cached_view_bindings.has_value()) {
                const auto it = std::ranges::find_if(
                    *expr.cached_view_bindings,
                    [](const ast::CachedViewBinding& cached) {
                        return cached.path.empty();
                    });
                if (it != expr.cached_view_bindings->end() &&
                    !it->element_sources.empty()) {
                    binding.source_places = it->element_sources;
                } else if (it != expr.cached_view_bindings->end()) {
                    binding.source_places = it->source_places;
                }
            }
            if (binding.source_places.empty() &&
                expr.resolved_place.has_value()) {
                const auto slot_index = findViewSlotLocal(
                    state, expr.resolved_place->is_external,
                    expr.resolved_place->root_id, expr.resolved_place->fields);
                if (slot_index.has_value()) {
                    binding.source_places =
                        topLevelOrigins(state.locals[*slot_index]);
                    binding.element_sources =
                        state.locals[*slot_index].element_origins;
                    if (!binding.source_local_id.has_value()) {
                        binding.source_local_id =
                            state.locals[*slot_index].unique_id;
                    }
                }
            }
            if (binding.source_places.empty()) {
                auto source_places = projectedPlaceTargets(state, expr);
                if (!source_places) {
                    return std::unexpected(source_places.error());
                }
                binding.source_places = std::move(*source_places);
            }
            if (binding.element_sources.empty()) {
                auto projected_sources = projectedPlaceSources(state, expr);
                if (!projected_sources) {
                    return std::unexpected(projected_sources.error());
                }
                binding.element_sources = std::move(*projected_sources);
            }
            if (binding.source_places.empty()) {
                auto place = borrowSourcePlace(state, expr);
                if (!place) {
                    return std::unexpected(place.error());
                }
                binding.source_places = {*place};
            }
        } else {
            if (expr.resolved_place.has_value()) {
                const auto slot_index = findViewSlotLocal(
                    state, expr.resolved_place->is_external,
                    expr.resolved_place->root_id, expr.resolved_place->fields);
                if (slot_index.has_value()) {
                    binding.source_local_id =
                        state.locals[*slot_index].unique_id;
                }
            }
            if (expr.resolved_place.has_value()) {
                const auto slot_index = findViewSlotLocal(
                    state, expr.resolved_place->is_external,
                    expr.resolved_place->root_id, expr.resolved_place->fields);
                if (slot_index.has_value()) {
                    binding.source_places =
                        topLevelOrigins(state.locals[*slot_index]);
                }
            }
            if (binding.source_places.empty()) {
                auto source_places = projectedPlaceTargets(state, expr);
                if (!source_places) {
                    return std::unexpected(source_places.error());
                }
                binding.source_places = std::move(*source_places);
            }
            if (binding.source_places.empty()) {
                auto place = sliceSourcePlace(state, expr);
                if (!place) {
                    return std::unexpected(place.error());
                }
                if (place->has_value()) {
                    binding.source_places = {**place};
                }
            }
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
            const auto has_return_dependencies = std::ranges::any_of(
                call->function->return_dependencies,
                [](const ast::ReturnDependency& dependency) {
                    return dependency.target.is_return;
                });
            if (!has_return_dependencies) {
                return unexpected_result<std::vector<ViewLeafBinding>>(
                    "could not determine aggregate borrow or slice sources",
                    expr.range);
            }
            std::vector<ViewLeafBinding> bindings;
            std::vector<std::vector<ViewLeafBinding>> cached_arguments(
                call->arguments.size());
            std::vector<bool> argument_ready(call->arguments.size(), false);
            std::vector<bool> include_projected_argument_bindings(
                call->arguments.size(), false);
            for (const auto& dependency : call->function->return_dependencies) {
                if (!dependency.target.is_return ||
                    !dependency.source.parameter_index.has_value() ||
                    dependency.source.resolved_path.empty()) {
                    continue;
                }
                include_projected_argument_bindings[*dependency.source
                                                         .parameter_index] =
                    true;
            }

            for (const auto& dependency : call->function->return_dependencies) {
                if (!dependency.target.is_return) {
                    continue;
                }
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
                    if (include_projected_argument_bindings[parameter_index]) {
                        auto extended = extendBindingsWithProjectedPointee(
                            state, *argument_bindings,
                            call->function->parameters[parameter_index]
                                .resolved_type,
                            false, expr.range);
                        if (!extended) {
                            return std::unexpected(extended.error());
                        }
                    }
                    cached_arguments[parameter_index] =
                        std::move(*argument_bindings);
                    argument_ready[parameter_index] = true;
                }

                auto binding = buildLeafDependencyBinding(
                    dependency, cached_arguments[parameter_index], true,
                    expr.range);
                if (!binding) {
                    return std::unexpected(binding.error());
                }
                bindings.push_back(std::move(*binding));
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

    const auto summarized_indexed_target =
        std::ranges::find(target_place.fields, INDEX_FIELD_SENTINEL) !=
        target_place.fields.end();

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
        std::vector<ast::ResolvedPlace> assigned_sources =
            binding->source_places;
        std::vector<ast::ResolvedPlace> assigned_element_sources =
            binding->element_sources;
        if (summarized_indexed_target) {
            assigned_sources =
                joinPlaces(topLevelOrigins(slot), assigned_sources);
            assigned_element_sources =
                joinPlaces(slot.element_origins, assigned_element_sources);
        }
        setTopLevelOrigins(slot, assigned_sources);
        slot.element_origins = assigned_element_sources;
        auto outlives = ensureViewSourceOutlivesLocal(state, slot, range);
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
                if (is_borrow_like_type(parent.type)) {
                    slot.reborrow_parent_local_id = parent.unique_id;
                    slot.is_interior_mut_borrow = !parent.type->is_mut;
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
    std::optional<std::size_t> parameter_index, bool allow_borrow_projection)
    -> std::expected<void, Diagnostic> {
    path.parameter_index = parameter_index;
    path.resolved_path.clear();

    const Type* current_type = root_type;
    for (const auto& segment : path.segments) {
        if (current_type == nullptr) {
            return make_error("invalid depends path", segment.range);
        }
        const Type* projected_type = current_type;
        const Type* current_base = types.unqualify(projected_type);
        while (allow_borrow_projection &&
               current_base->kind == TypeKind::Borrow &&
               current_base->element_type != nullptr) {
            projected_type = current_base->element_type;
            current_base = types.unqualify(projected_type);
        }
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
    const auto has_mut_view_parameter = std::ranges::any_of(
        decl.parameters, [&](const ast::Parameter& parameter) {
            return parameter.resolved_type != nullptr &&
                   parameter.resolved_type->kind == TypeKind::Borrow &&
                   parameter.resolved_type->is_mut &&
                   parameter.resolved_type->element_type != nullptr &&
                   typeContainsViews(parameter.resolved_type->element_type);
        });
    if (!decl.return_dependencies.empty() && !returns_views &&
        !has_mut_view_parameter) {
        return make_error("depends clause is only valid on functions whose "
                          "return type or mutable borrow parameters contain "
                          "a borrow or slice view",
                          decl.range);
    }
    if (!returns_views && !has_mut_view_parameter) {
        return {};
    }

    const auto return_leaves = collectViewLeafInfos(decl.resolved_return_type);

    if (returns_views && decl.return_dependencies.empty()) {
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

    struct SelectedDependencyLeaf {
        bool is_return = false;
        std::optional<std::size_t> parameter_index;
        std::vector<std::uint32_t> path;
        ast::ReturnDependency dependency;
        std::size_t specificity = 0;
    };
    std::vector<SelectedDependencyLeaf> selected_leaves;
    selected_leaves.reserve(return_leaves.size() +
                            decl.return_dependencies.size());

    for (auto& dependency : decl.return_dependencies) {
        if (dependency.target.is_return) {
            auto resolved_target = resolveDependencyPath(
                dependency.target, decl.resolved_return_type, std::nullopt);
            if (!resolved_target) {
                return std::unexpected(resolved_target.error());
            }
        } else {
            std::optional<std::size_t> target_parameter_index;
            const Type* target_root_type = nullptr;
            for (std::size_t index = 0; index < decl.parameters.size();
                 ++index) {
                if (decl.parameters[index].name !=
                    dependency.target.root_name) {
                    continue;
                }
                target_parameter_index = index;
                target_root_type = decl.parameters[index].resolved_type;
                break;
            }
            if (!target_parameter_index.has_value() ||
                target_root_type == nullptr) {
                return make_error("depends target '" +
                                      dependency.target.root_name +
                                      "' is not a parameter",
                                  dependency.target.range);
            }
            if (target_root_type->kind != TypeKind::Borrow ||
                !target_root_type->is_mut ||
                target_root_type->element_type == nullptr ||
                !typeContainsViews(target_root_type->element_type)) {
                return make_error("depends parameter target must be a mutable "
                                  "borrow parameter whose pointee contains a "
                                  "borrow or slice view",
                                  dependency.target.range);
            }
            auto resolved_target = resolveDependencyPath(
                dependency.target, target_root_type->element_type,
                target_parameter_index);
            if (!resolved_target) {
                return std::unexpected(resolved_target.error());
            }
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
            dependency.source, source_root_type, parameter_index, true);
        if (!resolved_source) {
            return std::unexpected(resolved_source.error());
        }

        const auto target_suffixes =
            collectViewLeafInfos(dependency.target.resolved_type);
        const auto source_suffixes =
            collectViewLeafInfos(dependency.source.resolved_type);
        const auto source_is_broadcast = source_suffixes.size() == 1;
        if (!source_is_broadcast &&
            target_suffixes.size() != source_suffixes.size()) {
            return make_error(
                "depends source must either provide a single borrow or slice "
                "leaf or match the target aggregate view structure",
                dependency.range);
        }
        if (!source_is_broadcast) {
            for (std::size_t index = 0; index < target_suffixes.size();
                 ++index) {
                if (target_suffixes[index].path ==
                    source_suffixes[index].path) {
                    continue;
                }
                return make_error("depends source and target must have "
                                  "matching aggregate view structure",
                                  dependency.range);
            }
        }
        const auto specificity = dependency.target.resolved_path.size();
        for (std::size_t index = 0; index < target_suffixes.size(); ++index) {
            const auto source_index = source_is_broadcast ? 0 : index;
            if (is_direct_shared_view_slice(types,
                                            target_suffixes[index].type)) {
                const auto* source_leaf_type =
                    source_suffixes[source_index].type;
                const auto* source_leaf_base =
                    source_leaf_type == nullptr
                        ? nullptr
                        : types.unqualify(source_leaf_type);
                const auto source_is_slice =
                    source_leaf_base != nullptr &&
                    (source_leaf_base->kind == TypeKind::Slice ||
                     (source_leaf_base->kind == TypeKind::Borrow &&
                      source_leaf_base->element_type != nullptr &&
                      types.unqualify(source_leaf_base->element_type)->kind ==
                          TypeKind::Slice));
                if (!source_is_slice) {
                    return make_error(
                        "shared-borrow slice returns must depend on an "
                        "incoming slice source",
                        dependency.range);
                }
            }

            auto full_target_path = dependency.target.resolved_path;
            full_target_path.insert(full_target_path.end(),
                                    target_suffixes[index].path.begin(),
                                    target_suffixes[index].path.end());
            auto full_source_path = dependency.source.resolved_path;
            full_source_path.insert(full_source_path.end(),
                                    source_suffixes[source_index].path.begin(),
                                    source_suffixes[source_index].path.end());

            ast::ReturnDependency leaf_dependency = dependency;
            leaf_dependency.target.resolved_path = full_target_path;
            leaf_dependency.target.resolved_type = target_suffixes[index].type;
            leaf_dependency.source.resolved_path = full_source_path;
            leaf_dependency.source.resolved_type =
                source_suffixes[source_index].type;

            const auto selected_it = std::ranges::find_if(
                selected_leaves, [&](const SelectedDependencyLeaf& leaf) {
                    return leaf.is_return == dependency.target.is_return &&
                           leaf.parameter_index ==
                               dependency.target.parameter_index &&
                           leaf.path == full_target_path;
                });
            if (selected_it == selected_leaves.end()) {
                selected_leaves.push_back(SelectedDependencyLeaf{
                    .is_return = dependency.target.is_return,
                    .parameter_index = dependency.target.parameter_index,
                    .path = std::move(full_target_path),
                    .dependency = std::move(leaf_dependency),
                    .specificity = specificity,
                });
                continue;
            }

            if (specificity < selected_it->specificity) {
                continue;
            }

            if (specificity == selected_it->specificity) {
                const auto same_source =
                    selected_it->dependency.source.parameter_index ==
                        leaf_dependency.source.parameter_index &&
                    selected_it->dependency.source.resolved_path ==
                        leaf_dependency.source.resolved_path;
                if (!same_source) {
                    return make_error(
                        "depends clause has conflicting mappings for the same "
                        "borrow or slice path",
                        dependency.range);
                }
                continue;
            }

            selected_it->dependency = std::move(leaf_dependency);
            selected_it->specificity = specificity;
        }
    }

    for (const auto& leaf : return_leaves) {
        const auto covered_it = std::ranges::find_if(
            selected_leaves, [&](const SelectedDependencyLeaf& covered_leaf) {
                return covered_leaf.is_return && covered_leaf.path == leaf.path;
            });
        if (covered_it == selected_leaves.end()) {
            return make_error("depends clause must cover every borrow and "
                              "slice path in the return type",
                              decl.range);
        }
    }

    decl.return_dependencies.clear();
    decl.return_dependencies.reserve(selected_leaves.size());
    for (auto& selected_leaf : selected_leaves) {
        decl.return_dependencies.push_back(std::move(selected_leaf.dependency));
    }

    return {};
}

} // namespace cyan

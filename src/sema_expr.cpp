#include "sema_detail.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>

namespace cyan {

using namespace detail;

namespace {

auto binary_op_spelling(ast::BinaryOp op) -> std::string_view {
    switch (op) {
    case ast::BinaryOp::Add:
        return "+";
    case ast::BinaryOp::Subtract:
        return "-";
    case ast::BinaryOp::Multiply:
        return "*";
    case ast::BinaryOp::Divide:
        return "/";
    case ast::BinaryOp::Remainder:
        return "%";
    case ast::BinaryOp::ShiftLeft:
        return "<<";
    case ast::BinaryOp::ShiftRight:
        return ">>";
    case ast::BinaryOp::BitwiseAnd:
        return "&";
    case ast::BinaryOp::BitwiseXor:
        return "^";
    case ast::BinaryOp::BitwiseOr:
        return "|";
    case ast::BinaryOp::Less:
        return "<";
    case ast::BinaryOp::LessEqual:
        return "<=";
    case ast::BinaryOp::Greater:
        return ">";
    case ast::BinaryOp::GreaterEqual:
        return ">=";
    case ast::BinaryOp::Equal:
        return "==";
    case ast::BinaryOp::NotEqual:
        return "!=";
    case ast::BinaryOp::LogicalAnd:
        return "&&";
    case ast::BinaryOp::LogicalOr:
        return "||";
    }
    return "?";
}

auto make_binary_operand_type_diagnostic(const TypeContext& types,
                                         const ast::Expr& expr,
                                         const ast::BinaryExpr& binary,
                                         std::string message,
                                         const Type* lhs_type,
                                         const Type* rhs_type) -> Diagnostic {
    Diagnostic diagnostic(std::move(message), expr.range);
    diagnostic.addNote("left operand has type '" + types.describe(lhs_type) +
                           "'",
                       binary.lhs->range);
    diagnostic.addNote("right operand has type '" + types.describe(rhs_type) +
                           "'",
                       binary.rhs->range);
    return diagnostic;
}

} // namespace

auto SemanticAnalyzer::analyzeExpr(FunctionState& state, ast::Expr& expr,
                                   const Type* expected_type)
    -> std::expected<const Type*, Diagnostic> {
    expr.interface_source_type = nullptr;
    expr.interface_impls.clear();
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

    const auto* const literal_expected_type =
        expected_type == nullptr ? nullptr : types.unqualify(expected_type);
    const auto parse_integer_literal_value =
        [](std::string_view text) -> llvm::APInt {
        const auto bit_width = static_cast<unsigned>(
            std::max<std::size_t>(64, text.size() * 4 + 1));
        return llvm::APInt(bit_width, llvm::StringRef(text.data(), text.size()),
                           10);
    };
    const auto integer_literal_fits_positive =
        [&](const llvm::APInt& value, const Type* target_type) -> bool {
        target_type =
            target_type == nullptr ? nullptr : types.unqualify(target_type);
        if (target_type == nullptr || target_type->kind != TypeKind::Integer) {
            return false;
        }
        const auto compare_width = std::max<unsigned>(
            value.getBitWidth(),
            static_cast<unsigned>(target_type->bit_width) + 1U);
        const auto widened = value.zextOrTrunc(compare_width);
        llvm::APInt upper_bound(compare_width, 1);
        upper_bound <<=
            target_type->bit_width - (target_type->is_signed ? 1U : 0U);
        return widened.ult(upper_bound);
    };
    const auto integer_literal_fits_negative_magnitude =
        [&](const llvm::APInt& value, const Type* target_type) -> bool {
        target_type =
            target_type == nullptr ? nullptr : types.unqualify(target_type);
        if (target_type == nullptr || target_type->kind != TypeKind::Integer ||
            !target_type->is_signed) {
            return false;
        }
        const auto compare_width = std::max<unsigned>(
            value.getBitWidth(),
            static_cast<unsigned>(target_type->bit_width) + 1U);
        const auto widened = value.zextOrTrunc(compare_width);
        llvm::APInt upper_bound(compare_width, 1);
        upper_bound <<= target_type->bit_width - 1U;
        return widened.ule(upper_bound);
    };
    const auto resolve_integer_literal_type =
        [&](const ast::IntegerLiteralExpr& literal)
        -> std::expected<const Type*, Diagnostic> {
        const auto value = parse_integer_literal_value(literal.text);
        if (literal_expected_type != nullptr &&
            literal_expected_type->kind == TypeKind::Integer) {
            if (!integer_literal_fits_positive(value, literal_expected_type)) {
                return unexpected_result<const Type*>(
                    "integer literal does not fit in expected type '" +
                        types.describe(literal_expected_type) + "'",
                    expr.range);
            }
            return literal_expected_type;
        }

        const auto* default_type = types.defaultIntegerType();
        if (!integer_literal_fits_positive(value, default_type)) {
            return unexpected_result<const Type*>(
                "integer literal does not fit in default type '" +
                    types.describe(default_type) + "'",
                expr.range);
        }
        return default_type;
    };

    return std::visit(
        Overloaded{
            [&](ast::IntegerLiteralExpr& literal)
                -> std::expected<const Type*, Diagnostic> {
                auto literal_type = resolve_integer_literal_type(literal);
                if (!literal_type) {
                    return std::unexpected(literal_type.error());
                }
                expr.resolved_type = *literal_type;
                expr.resolved_place.reset();
                return expr.resolved_type;
            },
            [&](ast::FloatLiteralExpr&)
                -> std::expected<const Type*, Diagnostic> {
                if (literal_expected_type != nullptr &&
                    literal_expected_type->kind == TypeKind::Float) {
                    expr.resolved_type = literal_expected_type;
                } else {
                    expr.resolved_type = types.defaultFloatType();
                }
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
                    if (!isDefinitelyLive(local.status)) {
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
                        return unexpected_result<const Type*>(
                            localStatusMessage(local), expr.range);
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
                               TypeKind::Slice) {
                        for (const auto& source_place :
                             topLevelOrigins(local)) {
                            if (source_place.is_external) {
                                continue;
                            }
                            const auto source_index =
                                findLocalById(state, source_place.root_id);
                            if (source_index.has_value()) {
                                const auto& source_local =
                                    state.locals[*source_index];
                                if (!source_local.in_scope ||
                                    !isDefinitelyLive(source_local.status)) {
                                    return unexpected_result<const Type*>(
                                        "slice source is no longer live",
                                        expr.range);
                                }
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
                    const auto* negate_expected_type =
                        literal_expected_type != nullptr &&
                                (types.isInteger(literal_expected_type) ||
                                 types.isFloat(literal_expected_type))
                            ? literal_expected_type
                            : nullptr;
                    const auto* literal = std::get_if<ast::IntegerLiteralExpr>(
                        &unary.operand->node);
                    const auto* magnitude_type =
                        literal != nullptr && negate_expected_type != nullptr &&
                                types.isSignedInteger(negate_expected_type) &&
                                integer_literal_fits_negative_magnitude(
                                    parse_integer_literal_value(literal->text),
                                    negate_expected_type)
                            ? types.integerType(negate_expected_type->bit_width,
                                                false)
                            : negate_expected_type;
                    auto operand_type =
                        requireReadable(state, *unary.operand, magnitude_type);
                    if (!operand_type) {
                        return std::unexpected(operand_type.error());
                    }
                    const auto* value_type = types.unqualify(*operand_type);
                    if (!types.isInteger(value_type) &&
                        !types.isFloat(value_type)) {
                        return unexpected_result<const Type*>(
                            "unary '-' requires an integer or floating-point "
                            "operand",
                            unary.operand->range);
                    }
                    expr.resolved_type =
                        literal != nullptr && negate_expected_type != nullptr &&
                                types.isSignedInteger(negate_expected_type) &&
                                integer_literal_fits_negative_magnitude(
                                    parse_integer_literal_value(literal->text),
                                    negate_expected_type)
                            ? negate_expected_type
                            : value_type;
                    expr.resolved_place.reset();
                    return expr.resolved_type;
                }
                case ast::UnaryOp::LogicalNot: {
                    auto operand_type = requireReadable(state, *unary.operand);
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
                case ast::UnaryOp::BitwiseNot: {
                    auto operand_type = requireReadable(
                        state, *unary.operand,
                        literal_expected_type != nullptr &&
                                types.isInteger(literal_expected_type)
                            ? literal_expected_type
                            : nullptr);
                    if (!operand_type) {
                        return std::unexpected(operand_type.error());
                    }
                    if (!types.isInteger(*operand_type)) {
                        return unexpected_result<const Type*>(
                            "unary '~' requires an integer operand",
                            unary.operand->range);
                    }
                    expr.resolved_type = types.unqualify(*operand_type);
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
                                *source_local_id;
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
                            if (!isDefinitelyLive(local.status)) {
                                return unexpected_result<const Type*>(
                                    localStatusMessage(local),
                                    unary.operand->range);
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
                const auto is_numeric_literal_like =
                    [](const ast::Expr& candidate) -> bool {
                    if (std::holds_alternative<ast::IntegerLiteralExpr>(
                            candidate.node) ||
                        std::holds_alternative<ast::FloatLiteralExpr>(
                            candidate.node)) {
                        return true;
                    }
                    const auto* unary =
                        std::get_if<ast::UnaryExpr>(&candidate.node);
                    if (unary == nullptr || unary->op != ast::UnaryOp::Negate) {
                        return false;
                    }
                    return std::holds_alternative<ast::IntegerLiteralExpr>(
                               unary->operand->node) ||
                           std::holds_alternative<ast::FloatLiteralExpr>(
                               unary->operand->node);
                };
                const auto expected_literal_type =
                    [&](const ast::Expr& candidate,
                        const Type* other_type) -> const Type* {
                    other_type = other_type == nullptr
                                     ? nullptr
                                     : types.unqualify(other_type);
                    if (other_type == nullptr) {
                        return nullptr;
                    }
                    if (is_numeric_literal_like(candidate) &&
                        types.isInteger(other_type) &&
                        (std::holds_alternative<ast::IntegerLiteralExpr>(
                             candidate.node) ||
                         (std::get_if<ast::UnaryExpr>(&candidate.node) !=
                              nullptr &&
                          std::get_if<ast::UnaryExpr>(&candidate.node)->op ==
                              ast::UnaryOp::Negate &&
                          std::holds_alternative<ast::IntegerLiteralExpr>(
                              std::get_if<ast::UnaryExpr>(&candidate.node)
                                  ->operand->node)))) {
                        return other_type;
                    }
                    if (is_numeric_literal_like(candidate) &&
                        types.isFloat(other_type)) {
                        return other_type;
                    }
                    return nullptr;
                };

                std::expected<const Type*, Diagnostic> lhs_type =
                    unexpected_result<const Type*>("unreachable", expr.range);
                std::expected<const Type*, Diagnostic> rhs_type =
                    unexpected_result<const Type*>("unreachable", expr.range);

                const auto lhs_is_literal =
                    is_numeric_literal_like(*binary.lhs);
                const auto rhs_is_literal =
                    is_numeric_literal_like(*binary.rhs);

                if (binary.op == ast::BinaryOp::ShiftLeft ||
                    binary.op == ast::BinaryOp::ShiftRight) {
                    const auto* lhs_expected_type =
                        std::holds_alternative<ast::IntegerLiteralExpr>(
                            binary.lhs->node) &&
                                literal_expected_type != nullptr &&
                                types.isInteger(literal_expected_type)
                            ? literal_expected_type
                            : nullptr;
                    lhs_type =
                        requireReadable(state, *binary.lhs, lhs_expected_type);
                    if (!lhs_type) {
                        return std::unexpected(lhs_type.error());
                    }
                    rhs_type = requireReadable(state, *binary.rhs);
                    if (!rhs_type) {
                        return std::unexpected(rhs_type.error());
                    }
                } else if (lhs_is_literal && !rhs_is_literal) {
                    rhs_type = requireReadable(state, *binary.rhs);
                    if (!rhs_type) {
                        return std::unexpected(rhs_type.error());
                    }
                    lhs_type = requireReadable(
                        state, *binary.lhs,
                        expected_literal_type(*binary.lhs, *rhs_type));
                    if (!lhs_type) {
                        return std::unexpected(lhs_type.error());
                    }
                } else {
                    lhs_type = requireReadable(state, *binary.lhs);
                    if (!lhs_type) {
                        return std::unexpected(lhs_type.error());
                    }
                    rhs_type = requireReadable(
                        state, *binary.rhs,
                        expected_literal_type(*binary.rhs, *lhs_type));
                    if (!rhs_type) {
                        return std::unexpected(rhs_type.error());
                    }
                }
                const auto* lhs_value_type = types.unqualify(*lhs_type);
                const auto* rhs_value_type = types.unqualify(*rhs_type);

                switch (binary.op) {
                case ast::BinaryOp::Add:
                case ast::BinaryOp::Subtract:
                case ast::BinaryOp::Multiply:
                case ast::BinaryOp::Divide:
                case ast::BinaryOp::Remainder: {
                    if (types.isInteger(lhs_value_type) &&
                        lhs_value_type == rhs_value_type) {
                        expr.resolved_type = lhs_value_type;
                        break;
                    }
                    if (types.isFloat(lhs_value_type) &&
                        lhs_value_type == rhs_value_type) {
                        expr.resolved_type = lhs_value_type;
                        break;
                    }
                    if (state.unchecked_depth > 0 &&
                        lhs_value_type->kind == TypeKind::Pointer &&
                        types.isInteger(rhs_value_type) &&
                        (types.unqualify(lhs_value_type->element_type) !=
                         types.voidType()) &&
                        (binary.op == ast::BinaryOp::Add ||
                         binary.op == ast::BinaryOp::Subtract)) {
                        expr.resolved_type = lhs_value_type;
                        break;
                    }
                    if (state.unchecked_depth > 0 &&
                        types.isInteger(lhs_value_type) &&
                        rhs_value_type->kind == TypeKind::Pointer &&
                        (types.unqualify(rhs_value_type->element_type) !=
                         types.voidType()) &&
                        binary.op == ast::BinaryOp::Add) {
                        expr.resolved_type = rhs_value_type;
                        break;
                    }
                    auto diagnostic = make_binary_operand_type_diagnostic(
                        types, expr, binary,
                        "arithmetic operands have incompatible types for '" +
                            std::string(binary_op_spelling(binary.op)) + "'",
                        lhs_value_type, rhs_value_type);
                    diagnostic.addHelp(
                        "arithmetic operators require matching integer "
                        "operands, matching floating-point operands, or "
                        "unchecked pointer arithmetic");
                    return unexpected_result<const Type*>(
                        std::move(diagnostic));
                }
                case ast::BinaryOp::ShiftLeft:
                case ast::BinaryOp::ShiftRight: {
                    if (!types.isInteger(lhs_value_type) ||
                        !types.isInteger(rhs_value_type)) {
                        auto diagnostic = make_binary_operand_type_diagnostic(
                            types, expr, binary,
                            "shift operands must both be integers for '" +
                                std::string(binary_op_spelling(binary.op)) +
                                "'",
                            lhs_value_type, rhs_value_type);
                        diagnostic.addHelp(
                            "shift operators require integer operands");
                        return unexpected_result<const Type*>(
                            std::move(diagnostic));
                    }
                    expr.resolved_type = lhs_value_type;
                    break;
                }
                case ast::BinaryOp::BitwiseAnd:
                case ast::BinaryOp::BitwiseXor:
                case ast::BinaryOp::BitwiseOr: {
                    if (!(types.isInteger(lhs_value_type) &&
                          lhs_value_type == rhs_value_type)) {
                        auto diagnostic = make_binary_operand_type_diagnostic(
                            types, expr, binary,
                            "bitwise operands have incompatible types for '" +
                                std::string(binary_op_spelling(binary.op)) +
                                "'",
                            lhs_value_type, rhs_value_type);
                        diagnostic.addHelp(
                            "bitwise operators require matching integer "
                            "operands");
                        return unexpected_result<const Type*>(
                            std::move(diagnostic));
                    }
                    expr.resolved_type = lhs_value_type;
                    break;
                }
                case ast::BinaryOp::Less:
                case ast::BinaryOp::LessEqual:
                case ast::BinaryOp::Greater:
                case ast::BinaryOp::GreaterEqual: {
                    if ((!types.isInteger(lhs_value_type) ||
                         lhs_value_type != rhs_value_type) &&
                        (!types.isFloat(lhs_value_type) ||
                         lhs_value_type != rhs_value_type)) {
                        auto diagnostic = make_binary_operand_type_diagnostic(
                            types, expr, binary,
                            "comparison operands have incompatible types for "
                            "'" +
                                std::string(binary_op_spelling(binary.op)) +
                                "'",
                            lhs_value_type, rhs_value_type);
                        diagnostic.addHelp(
                            "comparison operators require matching integer "
                            "or floating-point operands");
                        return unexpected_result<const Type*>(
                            std::move(diagnostic));
                    }
                    expr.resolved_type = types.boolType();
                    break;
                }
                case ast::BinaryOp::Equal:
                case ast::BinaryOp::NotEqual: {
                    if (!can_consume_value_type(types, *lhs_type, *rhs_type) &&
                        !can_consume_value_type(types, *rhs_type, *lhs_type)) {
                        auto diagnostic = make_binary_operand_type_diagnostic(
                            types, expr, binary,
                            "equality operands have incompatible types for '" +
                                std::string(binary_op_spelling(binary.op)) +
                                "'",
                            *lhs_type, *rhs_type);
                        diagnostic.addHelp(
                            "equality operands must have the same type");
                        return unexpected_result<const Type*>(
                            std::move(diagnostic));
                    }
                    if (!types.isCopy(lhs_value_type)) {
                        return unexpected_result<const Type*>(
                            "equality is only implemented for copy types in "
                            "the core MVP",
                            expr.range);
                    }
                    expr.resolved_type = types.boolType();
                    break;
                }
                case ast::BinaryOp::LogicalAnd:
                case ast::BinaryOp::LogicalOr: {
                    if (lhs_value_type != types.boolType() ||
                        rhs_value_type != types.boolType()) {
                        auto diagnostic = make_binary_operand_type_diagnostic(
                            types, expr, binary,
                            "logical operands must both be 'bool' for '" +
                                std::string(binary_op_spelling(binary.op)) +
                                "'",
                            lhs_value_type, rhs_value_type);
                        diagnostic.addHelp(
                            "logical operators require bool operands");
                        return unexpected_result<const Type*>(
                            std::move(diagnostic));
                    }
                    expr.resolved_type = types.boolType();
                    break;
                }
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
                auto index_type = requireReadable(state, *index_expr.index);
                if (!index_type) {
                    return std::unexpected(index_type.error());
                }
                if (!types.isInteger(*index_type)) {
                    return unexpected_result<const Type*>(
                        "index expression must have an integer type",
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
                                    .source_places = binding_it->source_places,
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
                auto move_existing_borrow_value = [&](ast::Expr& value_expr,
                                                      const Type* borrow_type)
                    -> std::expected<void, Diagnostic> {
                    if (types.isCopy(borrow_type)) {
                        return {};
                    }
                    auto source_local_id =
                        borrowSourceLocalId(state, value_expr);
                    if (!source_local_id) {
                        return std::unexpected(source_local_id.error());
                    }
                    if (!source_local_id->has_value()) {
                        return {};
                    }
                    const auto local_index =
                        findLocalById(state, **source_local_id);
                    if (!local_index.has_value()) {
                        return {};
                    }
                    auto& local = state.locals[*local_index];
                    if (local.type == nullptr ||
                        !types.isSame(local.type, borrow_type)) {
                        return {};
                    }
                    if (local.type->is_mut) {
                        for (const auto& origin_place :
                             topLevelOrigins(local)) {
                            auto borrow = ensureCanBorrow(
                                state, origin_place, true, value_expr.range,
                                local.unique_id);
                            if (!borrow) {
                                return std::unexpected(borrow.error());
                            }
                        }
                    }
                    releaseReborrowParent(state, local);
                    local.status = LocalState::Status::Moved;
                    markAggregateViewSlots(state, localPlace(local.unique_id),
                                           local.type,
                                           LocalState::Status::Moved);
                    return {};
                };
                for (std::size_t index = 0; index < init_list.elements.size();
                     ++index) {
                    auto& element_expr = *init_list.elements[index];
                    const auto* field_type = fields[index].resolved_type;
                    if (field_type == nullptr) {
                        return unexpected_result<const Type*>(
                            "initializer field type is unresolved",
                            element_expr.range);
                    }
                    bool move_existing_borrow = false;

                    if (field_type->kind == TypeKind::Borrow) {
                        auto analyzed =
                            analyzeExpr(state, element_expr, field_type);
                        if (!analyzed) {
                            return std::unexpected(analyzed.error());
                        }
                        auto source_local_id =
                            borrowSourceLocalId(state, element_expr);
                        if (!source_local_id) {
                            return std::unexpected(source_local_id.error());
                        }
                        const auto* unary =
                            std::get_if<ast::UnaryExpr>(&element_expr.node);
                        const auto explicit_borrow_syntax =
                            unary != nullptr &&
                            (unary->op == ast::UnaryOp::Borrow ||
                             unary->op == ast::UnaryOp::BorrowMut);
                        const auto existing_borrow_value =
                            types.isSame(*analyzed, field_type) &&
                            !explicit_borrow_syntax &&
                            (source_local_id->has_value() ||
                             element_expr.cached_view_bindings.has_value() ||
                             (element_expr.resolved_place.has_value() &&
                              element_expr.resolved_place->owner_local_id
                                  .has_value()));
                        if (!types.isSame(*analyzed, field_type) ||
                            !existing_borrow_value) {
                            auto borrowed = borrowFromExpr(
                                state, element_expr, field_type->is_mut, true);
                            if (!borrowed) {
                                return std::unexpected(borrowed.error());
                            }
                            analyzed =
                                analyzeExpr(state, element_expr, field_type);
                            if (!analyzed) {
                                return std::unexpected(analyzed.error());
                            }
                        }
                        if (!types.isSame(element_expr.resolved_type,
                                          field_type)) {
                            return unexpected_result<const Type*>(
                                "initializer field type does not match",
                                element_expr.range);
                        }
                        move_existing_borrow =
                            existing_borrow_value && !types.isCopy(field_type);
                    }

                    if (typeContainsViews(field_type)) {
                        auto analyzed =
                            analyzeExpr(state, element_expr, field_type);
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
                                .source_places = binding.source_places,
                                .source_local_id = binding.source_local_id,
                                .element_sources = binding.element_sources,
                                .type = binding.type,
                            });
                        }
                    }

                    if (field_type->kind != TypeKind::Borrow) {
                        auto element =
                            consumeValue(state, element_expr, field_type);
                        if (!element) {
                            return std::unexpected(element.error());
                        }
                    } else if (move_existing_borrow) {
                        auto moved = move_existing_borrow_value(element_expr,
                                                                field_type);
                        if (!moved) {
                            return std::unexpected(moved.error());
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
                const bool track_array_view_bindings =
                    expected_base->kind == TypeKind::Array &&
                    typeContainsViews(expected_base->element_type);
                const bool track_element_sources =
                    expected_base->kind == TypeKind::Slice &&
                    is_direct_shared_view_slice(types, expected_type);
                std::vector<ast::ResolvedPlace> element_sources;
                std::vector<ast::CachedViewBinding> cached_bindings;
                auto merge_cached_binding =
                    [&](ast::CachedViewBinding next_binding) -> void {
                    const auto existing_it = std::ranges::find_if(
                        cached_bindings,
                        [&](const ast::CachedViewBinding& binding) {
                            return binding.path == next_binding.path;
                        });
                    if (existing_it == cached_bindings.end()) {
                        cached_bindings.push_back(std::move(next_binding));
                        return;
                    }

                    auto append_unique_place =
                        [](std::vector<ast::ResolvedPlace>& places,
                           const ast::ResolvedPlace& place) -> void {
                        if (std::ranges::find(places, place) == places.end()) {
                            places.push_back(place);
                        }
                    };
                    for (const auto& place : next_binding.source_places) {
                        append_unique_place(existing_it->source_places, place);
                    }
                    for (const auto& place : next_binding.element_sources) {
                        append_unique_place(existing_it->element_sources,
                                            place);
                    }
                    if (!existing_it->source_local_id.has_value()) {
                        existing_it->source_local_id =
                            next_binding.source_local_id;
                    } else if (next_binding.source_local_id.has_value() &&
                               existing_it->source_local_id !=
                                   next_binding.source_local_id) {
                        existing_it->source_local_id.reset();
                    }
                };

                for (auto& element : array_literal.elements) {
                    if (track_array_view_bindings) {
                        auto analyzed = analyzeExpr(
                            state, *element, expected_base->element_type);
                        if (!analyzed) {
                            return std::unexpected(analyzed.error());
                        }
                        auto bindings =
                            collectExprViewBindings(state, *element);
                        if (!bindings) {
                            return std::unexpected(bindings.error());
                        }
                        for (const auto& binding : *bindings) {
                            auto path = binding.path;
                            path.insert(path.begin(), INDEX_FIELD_SENTINEL);
                            merge_cached_binding(ast::CachedViewBinding{
                                .path = std::move(path),
                                .source_places = binding.source_places,
                                .source_local_id = binding.source_local_id,
                                .element_sources = binding.element_sources,
                                .type = binding.type,
                            });
                        }
                    }
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
                    merge_cached_binding(ast::CachedViewBinding{
                        .path = {},
                        .source_places =
                            expr.slice_source_place.has_value()
                                ? std::vector<
                                      ast::
                                          ResolvedPlace>{*expr.slice_source_place}
                                : std::vector<ast::ResolvedPlace>{},
                        .source_local_id = std::nullopt,
                        .element_sources = std::move(element_sources),
                        .type = expected_type,
                    });
                }
                if (track_array_view_bindings || !cached_bindings.empty()) {
                    expr.cached_view_bindings = std::move(cached_bindings);
                }
                return expr.resolved_type;
            },
            [&](ast::CastExpr& cast_expr)
                -> std::expected<const Type*, Diagnostic> {
                auto target_type = resolveType(*cast_expr.target_type);
                if (!target_type) {
                    return std::unexpected(target_type.error());
                }
                const auto literal_cast_target =
                    (std::holds_alternative<ast::IntegerLiteralExpr>(
                         cast_expr.operand->node) ||
                     std::holds_alternative<ast::FloatLiteralExpr>(
                         cast_expr.operand->node) ||
                     (std::get_if<ast::UnaryExpr>(&cast_expr.operand->node) !=
                          nullptr &&
                      std::get_if<ast::UnaryExpr>(&cast_expr.operand->node)
                              ->op == ast::UnaryOp::Negate)) &&
                            (types.isInteger(*target_type) ||
                             types.isFloat(*target_type))
                        ? *target_type
                        : nullptr;
                auto operand_type = requireReadable(state, *cast_expr.operand,
                                                    literal_cast_target);
                if (!operand_type) {
                    return std::unexpected(operand_type.error());
                }
                if (cast_expr.owner_expr != nullptr) {
                    if (state.unchecked_depth == 0) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow casts are only allowed in "
                            "unchecked blocks",
                            expr.range);
                    }
                    if ((*target_type)->kind != TypeKind::Borrow) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow casts require a borrow target "
                            "type",
                            expr.range);
                    }
                    const auto* pointer_type = types.unqualify(*operand_type);
                    if (pointer_type->kind != TypeKind::Pointer ||
                        pointer_type->element_type == nullptr) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow casts require a pointer "
                            "operand",
                            cast_expr.operand->range);
                    }
                    if ((*target_type)->is_mut &&
                        pointer_type->element_type->is_const) {
                        return unexpected_result<const Type*>(
                            "cannot mutably borrow through a const pointer",
                            expr.range);
                    }
                    if (!types.sameIgnoringTopLevelConst(
                            pointer_type->element_type,
                            (*target_type)->element_type)) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow cast requires matching "
                            "pointee type",
                            expr.range);
                    }

                    auto owner_type = analyzeExpr(state, *cast_expr.owner_expr);
                    if (!owner_type) {
                        return std::unexpected(owner_type.error());
                    }
                    if (!is_borrow_like_type(*owner_type)) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow casts require a borrow owner",
                            cast_expr.owner_expr->range);
                    }

                    auto owner_bindings =
                        collectExprViewBindings(state, *cast_expr.owner_expr);
                    if (!owner_bindings) {
                        return std::unexpected(owner_bindings.error());
                    }
                    const auto top_level_binding = std::ranges::find_if(
                        *owner_bindings, [](const ViewLeafBinding& binding) {
                            return binding.path.empty();
                        });
                    if (top_level_binding == owner_bindings->end() ||
                        top_level_binding->source_places.empty()) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow cast owner has no tracked "
                            "source",
                            cast_expr.owner_expr->range);
                    }

                    cast_expr.cast_kind = ast::CastKind::OwnerBorrow;
                    expr.resolved_type = *target_type;
                    expr.cached_view_bindings =
                        std::vector<ast::CachedViewBinding>{
                            ast::CachedViewBinding{
                                .path = {},
                                .source_places =
                                    top_level_binding->source_places,
                                .source_local_id =
                                    top_level_binding->source_local_id,
                                .element_sources = {},
                                .type = expr.resolved_type,
                            },
                        };
                    expr.resolved_place = placeSetRepresentative(
                        top_level_binding->source_places);
                    if (!expr.resolved_place.has_value()) {
                        return unexpected_result<const Type*>(
                            "owner-bound borrow cast owner has no tracked "
                            "source",
                            cast_expr.owner_expr->range);
                    }
                    if (expr.resolved_type->is_mut &&
                        top_level_binding->source_local_id.has_value()) {
                        expr.resolved_place->owner_local_id =
                            top_level_binding->source_local_id;
                    }
                    expr.slice_source_place.reset();
                    return expr.resolved_type;
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
                    types.isInteger(*operand_type) ||
                    types.isFloat(*operand_type) ||
                    types.unqualify(*operand_type) == types.charType();
                const auto target_numeric =
                    types.isInteger(*target_type) ||
                    types.isFloat(*target_type) ||
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
                expr.resolved_type = types.i64Type();
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
    enum class AtomicOrderUse : std::uint8_t {
        Load,
        Store,
        Rmw,
        CompareExchangeSuccess,
        CompareExchangeFailure,
        Fence,
    };
    const auto is_atomic_order_kind = [](ast::BuiltinCallKind kind) -> bool {
        switch (kind) {
        case ast::BuiltinCallKind::AtomicRelaxedOrder:
        case ast::BuiltinCallKind::AtomicAcquireOrder:
        case ast::BuiltinCallKind::AtomicReleaseOrder:
        case ast::BuiltinCallKind::AtomicAcqRelOrder:
        case ast::BuiltinCallKind::AtomicSeqCstOrder:
            return true;
        default:
            return false;
        }
    };
    const auto atomic_order_label =
        [](ast::BuiltinCallKind kind) -> std::string_view {
        switch (kind) {
        case ast::BuiltinCallKind::AtomicRelaxedOrder:
            return "relaxed";
        case ast::BuiltinCallKind::AtomicAcquireOrder:
            return "acquire";
        case ast::BuiltinCallKind::AtomicReleaseOrder:
            return "release";
        case ast::BuiltinCallKind::AtomicAcqRelOrder:
            return "acq_rel";
        case ast::BuiltinCallKind::AtomicSeqCstOrder:
            return "seq_cst";
        default:
            return "invalid";
        }
    };
    const auto is_atomic_scalar_type = [&](const Type* type) -> bool {
        type = types.unqualify(type);
        return type != nullptr && (type->kind == TypeKind::Integer ||
                                   type->kind == TypeKind::Bool ||
                                   type->kind == TypeKind::Pointer);
    };
    const auto require_atomic_pointer =
        [&](ast::Expr& pointer_expr,
            bool require_writable) -> std::expected<const Type*, Diagnostic> {
        auto pointer_type = requireReadable(state, pointer_expr);
        if (!pointer_type) {
            return std::unexpected(pointer_type.error());
        }
        const auto* pointer_base = types.unqualify(*pointer_type);
        if (pointer_base->kind != TypeKind::Pointer ||
            !pointer_base->is_shared || pointer_base->element_type == nullptr) {
            return unexpected_result<const Type*>(
                "atomic operations require a shared pointer operand",
                pointer_expr.range);
        }
        if (require_writable && pointer_base->element_type->is_const) {
            return unexpected_result<const Type*>(
                "atomic write operations require a mutable pointee type",
                pointer_expr.range);
        }
        return pointer_base;
    };
    const auto require_atomic_order = [&](ast::Expr& order_expr,
                                          AtomicOrderUse use)
        -> std::expected<ast::BuiltinCallKind, Diagnostic> {
        auto order_type = requireReadable(state, order_expr, types.i64Type());
        if (!order_type) {
            return std::unexpected(order_type.error());
        }
        const auto* order_call = std::get_if<ast::CallExpr>(&order_expr.node);
        if (order_call == nullptr ||
            !is_atomic_order_kind(order_call->builtin_kind)) {
            return unexpected_result<ast::BuiltinCallKind>(
                "atomic memory order must be one of atomic_relaxed(), "
                "atomic_acquire(), atomic_release(), atomic_acq_rel(), or "
                "atomic_seq_cst()",
                order_expr.range);
        }

        const auto valid = [=](ast::BuiltinCallKind kind) -> bool {
            switch (use) {
            case AtomicOrderUse::Load:
                return kind == ast::BuiltinCallKind::AtomicRelaxedOrder ||
                       kind == ast::BuiltinCallKind::AtomicAcquireOrder ||
                       kind == ast::BuiltinCallKind::AtomicSeqCstOrder;
            case AtomicOrderUse::Store:
                return kind == ast::BuiltinCallKind::AtomicRelaxedOrder ||
                       kind == ast::BuiltinCallKind::AtomicReleaseOrder ||
                       kind == ast::BuiltinCallKind::AtomicSeqCstOrder;
            case AtomicOrderUse::Rmw:
            case AtomicOrderUse::CompareExchangeSuccess:
                return true;
            case AtomicOrderUse::CompareExchangeFailure:
                return kind == ast::BuiltinCallKind::AtomicRelaxedOrder ||
                       kind == ast::BuiltinCallKind::AtomicAcquireOrder ||
                       kind == ast::BuiltinCallKind::AtomicSeqCstOrder;
            case AtomicOrderUse::Fence:
                return kind == ast::BuiltinCallKind::AtomicAcquireOrder ||
                       kind == ast::BuiltinCallKind::AtomicReleaseOrder ||
                       kind == ast::BuiltinCallKind::AtomicAcqRelOrder ||
                       kind == ast::BuiltinCallKind::AtomicSeqCstOrder;
            }
            return false;
        };
        if (!valid(order_call->builtin_kind)) {
            std::string op_name = "atomic operation";
            switch (use) {
            case AtomicOrderUse::Load:
                op_name = "atomic_load()";
                break;
            case AtomicOrderUse::Store:
                op_name = "atomic_store()";
                break;
            case AtomicOrderUse::Rmw:
                op_name = "atomic read-modify-write operation";
                break;
            case AtomicOrderUse::CompareExchangeSuccess:
                op_name = "atomic_compare_exchange() success";
                break;
            case AtomicOrderUse::CompareExchangeFailure:
                op_name = "atomic_compare_exchange() failure";
                break;
            case AtomicOrderUse::Fence:
                op_name = "atomic_fence()";
                break;
            }
            return unexpected_result<ast::BuiltinCallKind>(
                op_name + " does not accept " +
                    std::string(atomic_order_label(order_call->builtin_kind)) +
                    " ordering",
                order_expr.range);
        }
        return order_call->builtin_kind;
    };
    const auto validate_compare_exchange_orders =
        [&](ast::BuiltinCallKind success_order,
            ast::BuiltinCallKind failure_order,
            SourceRange range) -> std::expected<void, Diagnostic> {
        switch (success_order) {
        case ast::BuiltinCallKind::AtomicRelaxedOrder:
            if (failure_order != ast::BuiltinCallKind::AtomicRelaxedOrder) {
                return unexpected_result<void>(
                    "atomic_compare_exchange() failure ordering must not be "
                    "stronger than success ordering",
                    range);
            }
            return {};
        case ast::BuiltinCallKind::AtomicAcquireOrder:
            if (failure_order != ast::BuiltinCallKind::AtomicRelaxedOrder &&
                failure_order != ast::BuiltinCallKind::AtomicAcquireOrder) {
                return unexpected_result<void>(
                    "atomic_compare_exchange() failure ordering must not be "
                    "stronger than success ordering",
                    range);
            }
            return {};
        case ast::BuiltinCallKind::AtomicReleaseOrder:
            if (failure_order != ast::BuiltinCallKind::AtomicRelaxedOrder) {
                return unexpected_result<void>(
                    "atomic_compare_exchange() failure ordering must not be "
                    "stronger than success ordering",
                    range);
            }
            return {};
        case ast::BuiltinCallKind::AtomicAcqRelOrder:
            if (failure_order != ast::BuiltinCallKind::AtomicRelaxedOrder &&
                failure_order != ast::BuiltinCallKind::AtomicAcquireOrder) {
                return unexpected_result<void>(
                    "atomic_compare_exchange() failure ordering must not be "
                    "stronger than success ordering",
                    range);
            }
            return {};
        case ast::BuiltinCallKind::AtomicSeqCstOrder:
            return {};
        default:
            return unexpected_result<void>("invalid compare-exchange ordering",
                                           range);
        }
    };
    struct ScopeExit {
        std::function<void()> fn;

        ~ScopeExit() { fn(); }
    };
    const auto temporary_loan_base = state.temporary_loans.size();
    const auto temporary_suspended_base =
        state.temporary_suspended_local_ids.size();
    const auto clear_call_temporaries = [&]() -> void {
        const auto has_live_reborrow_child = [&](std::size_t local_id) -> bool {
            return std::ranges::any_of(
                state.locals, [&](const LocalState& local) {
                    return local.in_scope && isDefinitelyLive(local.status) &&
                           local.reborrow_parent_local_id == local_id;
                });
        };

        for (std::size_t index = temporary_suspended_base;
             index < state.temporary_suspended_local_ids.size(); ++index) {
            const auto local_id = state.temporary_suspended_local_ids[index];
            const auto local_index = findLocalById(state, local_id);
            if (local_index.has_value() &&
                state.locals[*local_index].in_scope &&
                !has_live_reborrow_child(local_id)) {
                state.locals[*local_index].status = LocalState::Status::Live;
            }
        }
        state.temporary_suspended_local_ids.resize(temporary_suspended_base);
        state.temporary_loans.resize(temporary_loan_base);
    };
    [[maybe_unused]] const auto call_temporary_cleanup =
        ScopeExit{[&]() { clear_call_temporaries(); }};

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
                const auto local_origins = topLevelOrigins(local);
                if (parameter_type->is_mut) {
                    if (local.type->kind != TypeKind::Borrow ||
                        !local.type->is_mut || local_origins.empty()) {
                        return unexpected_result<void>(
                            "expected a mutable borrow argument",
                            argument.range);
                    }
                    for (const auto& origin : local_origins) {
                        auto borrow =
                            ensureCanBorrow(state, origin, true, argument.range,
                                            local.unique_id);
                        if (!borrow) {
                            return std::unexpected(borrow.error());
                        }
                    }
                    local.status = LocalState::Status::Moved;
                    for (auto loan_place : local_origins) {
                        loan_place.owner_local_id = local.unique_id;
                        state.temporary_loans.push_back(TemporaryLoan{
                            .place = loan_place,
                            .is_mut = true,
                            .range = argument.range,
                        });
                    }
                    state.temporary_suspended_local_ids.push_back(
                        local.unique_id);
                } else if (local.type->kind == TypeKind::Borrow &&
                           local.type->is_mut && !local_origins.empty()) {
                    for (const auto& origin : local_origins) {
                        auto borrow =
                            ensureCanBorrow(state, origin, false,
                                            argument.range, local.unique_id);
                        if (!borrow) {
                            return std::unexpected(borrow.error());
                        }
                    }
                    for (const auto& loan_place : local_origins) {
                        state.temporary_loans.push_back(TemporaryLoan{
                            .place = loan_place,
                            .is_mut = false,
                            .range = argument.range,
                        });
                    }
                } else if (local.type->kind != TypeKind::Borrow ||
                           local_origins.empty()) {
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
                        auto local_origins = topLevelOrigins(local);
                        if (local.type->kind == TypeKind::Interface &&
                            !local_origins.empty()) {
                            if (parameter_type->is_mut) {
                                for (const auto& origin : local_origins) {
                                    auto borrow = ensureCanBorrow(
                                        state, origin, true, argument.range,
                                        local.unique_id);
                                    if (!borrow) {
                                        return std::unexpected(borrow.error());
                                    }
                                }
                                local.status = LocalState::Status::Moved;
                                for (auto loan_place : local_origins) {
                                    loan_place.owner_local_id = local.unique_id;
                                    state.temporary_loans.push_back(
                                        TemporaryLoan{.place = loan_place,
                                                      .is_mut = true,
                                                      .range = argument.range});
                                }
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
                        .range = argument.range,
                    });
                }
                continue;
            }
            if (binding.source_places.empty()) {
                continue;
            }
            const auto binding_is_mut =
                is_borrow_like_type(binding.type) && binding.type->is_mut;
            for (auto loan_place : binding.source_places) {
                if (binding_is_mut && binding.source_local_id.has_value()) {
                    loan_place.owner_local_id = binding.source_local_id;
                }
                state.temporary_loans.push_back(TemporaryLoan{
                    .place = std::move(loan_place),
                    .is_mut = binding_is_mut,
                    .range = argument.range,
                });
            }
        }
        return {};
    };

    auto borrow_argument_place = [&](ast::Expr& argument)
        -> std::expected<ast::ResolvedPlace, Diagnostic> {
        const Type* argument_type = argument.resolved_type;
        if (argument_type == nullptr) {
            auto analyzed_argument = analyzeExpr(state, argument);
            if (!analyzed_argument) {
                return std::unexpected(analyzed_argument.error());
            }
            argument_type = *analyzed_argument;
        }
        if (is_borrow_like_type(argument_type)) {
            return borrowSourcePlace(state, argument);
        }
        return resolvePlace(state, argument);
    };

    auto invalidate_view_subtree = [&](const ast::ResolvedPlace& base_place,
                                       const Type* type) -> void {
        if (!typeContainsViews(type) || base_place.root_id == 0) {
            return;
        }
        for (const auto& leaf : collectViewLeafInfos(type)) {
            auto full_path = base_place.fields;
            full_path.insert(full_path.end(), leaf.path.begin(),
                             leaf.path.end());
            const auto slot_index = findViewSlotLocal(
                state, base_place.is_external, base_place.root_id, full_path);
            if (!slot_index.has_value()) {
                continue;
            }
            auto& slot = state.locals[*slot_index];
            releaseReborrowParent(state, slot);
            slot.borrow_origins.clear();
            slot.element_origins.clear();
            slot.status = LocalState::Status::Unavailable;
        }
    };

    ast::FunctionDecl* function = nullptr;
    const auto callee_is_qualified = call.callee.find('.') != std::string::npos;
    const ast::FunctionDecl* builtin_decl = nullptr;
    ast::BuiltinCallKind builtin_decl_kind = ast::BuiltinCallKind::None;
    if (auto* visible_function = findVisibleFunction(call.callee);
        visible_function != nullptr &&
        visible_function->builtin_lowering.has_value()) {
        builtin_decl = visible_function;
        builtin_decl_kind = visible_function->builtin_lowering->builtin_kind;
    } else if (auto* visible_template =
                   findVisibleFunctionTemplate(call.callee);
               visible_template != nullptr &&
               visible_template->builtin_lowering.has_value()) {
        if (call.explicit_type_arguments.empty()) {
            builtin_decl = visible_template;
            builtin_decl_kind =
                visible_template->builtin_lowering->builtin_kind;
        } else {
            TypeBindings type_bindings;
            if (call.explicit_type_arguments.size() !=
                visible_template->type_parameters.size()) {
                return unexpected_result<const Type*>(
                    "wrong number of explicit type arguments for '" +
                        call.callee + "'",
                    expr.range);
            }
            for (std::size_t index = 0;
                 index < call.explicit_type_arguments.size(); ++index) {
                auto resolved_type =
                    resolveType(*call.explicit_type_arguments[index]);
                if (!resolved_type) {
                    return std::unexpected(resolved_type.error());
                }
                type_bindings.emplace(visible_template->type_parameters[index],
                                      *resolved_type);
            }

            auto instantiated =
                instantiateFunctionTemplate(*visible_template, type_bindings);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            function = *instantiated;
            builtin_decl = function;
            builtin_decl_kind = function->builtin_lowering->builtin_kind;

            auto ensured_signature = ensureFunctionSignature(*function);
            if (!ensured_signature) {
                return std::unexpected(ensured_signature.error());
            }
            if (function->signature_status ==
                ast::FunctionDecl::SignatureStatus::Invalid) {
                expr.resolved_type = function->resolved_return_type != nullptr
                                         ? function->resolved_return_type
                                         : types.voidType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            }
            if (function->analysis_failed) {
                expr.resolved_type = function->resolved_return_type != nullptr
                                         ? function->resolved_return_type
                                         : types.voidType();
                expr.resolved_place.reset();
                return expr.resolved_type;
            }
            if (function->resolved_return_type == nullptr ||
                std::ranges::any_of(
                    function->parameters, [](const ast::Parameter& parameter) {
                        return parameter.resolved_type == nullptr;
                    })) {
                return unexpected_result<const Type*>(
                    "cannot call a function with an invalid type signature",
                    expr.range);
            }

            if (call.arguments.size() == function->parameters.size()) {
                for (std::size_t index = 0; index < call.arguments.size();
                     ++index) {
                    auto prechecked_argument =
                        analyzeExpr(state, *call.arguments[index],
                                    function->parameters[index].resolved_type);
                    if (!prechecked_argument) {
                        return std::unexpected(prechecked_argument.error());
                    }
                }
            }
        }
    }
    const auto builtin_available =
        !callee_is_qualified && findVisibleFunction(call.callee) == nullptr &&
        findVisibleFunctionTemplate(call.callee) == nullptr &&
        findVisibleInterface(call.callee) == nullptr &&
        !findVisibleEnumVariant(call.callee).has_value() &&
        findVisibleTemplateEnumVariants(call.callee).empty();
    const auto bind_builtin_decl = [&]() -> void {
        if (builtin_decl != nullptr) {
            call.function = builtin_decl;
        }
    };
    const auto builtin_decl_is_template_instance =
        builtin_decl != nullptr && builtin_decl->template_decl != nullptr;
    if (((call.callee == "__builtin_atomic_relaxed" ||
          call.callee == "__builtin_atomic_acquire" ||
          call.callee == "__builtin_atomic_release" ||
          call.callee == "__builtin_atomic_acq_rel" ||
          call.callee == "__builtin_atomic_seq_cst") &&
         builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicRelaxedOrder ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicAcquireOrder ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicReleaseOrder ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicAcqRelOrder ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicSeqCstOrder) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (!call.arguments.empty()) {
            return unexpected_result<const Type*>(
                std::string(builtin_decl != nullptr ? builtin_decl->name
                                                    : "atomic order") +
                    "() expects exactly zero arguments",
                expr.range);
        }
        if (builtin_decl_kind != ast::BuiltinCallKind::None) {
            call.builtin_kind = builtin_decl_kind;
        } else if (call.callee == "__builtin_atomic_relaxed") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicRelaxedOrder;
        } else if (call.callee == "__builtin_atomic_acquire") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicAcquireOrder;
        } else if (call.callee == "__builtin_atomic_release") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicReleaseOrder;
        } else if (call.callee == "__builtin_atomic_acq_rel") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicAcqRelOrder;
        } else {
            call.builtin_kind = ast::BuiltinCallKind::AtomicSeqCstOrder;
        }
        bind_builtin_decl();
        expr.resolved_type = types.i64Type();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_len") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::Len) {
        if (call.arguments.size() != 1) {
            return unexpected_result<const Type*>(
                "len() expects exactly one argument", expr.range);
        }

        auto argument_type = requireReadable(state, *call.arguments.front());
        if (!argument_type) {
            return std::unexpected(argument_type.error());
        }
        const auto* argument_base = types.unqualify(*argument_type);
        if (argument_base->kind != TypeKind::Array &&
            argument_base->kind != TypeKind::Slice) {
            return unexpected_result<const Type*>(
                "len() requires an array or slice argument", expr.range);
        }

        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::Len;
        expr.resolved_type = types.i64Type();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_raw_data") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::RawData) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 1) {
            return unexpected_result<const Type*>(
                "raw_data() expects exactly one argument", expr.range);
        }

        auto argument_type = requireReadable(state, *call.arguments.front());
        if (!argument_type) {
            return std::unexpected(argument_type.error());
        }
        const auto* argument_base = types.unqualify(*argument_type);
        if (argument_base->kind != TypeKind::Slice) {
            return unexpected_result<const Type*>(
                "raw_data() requires a slice argument", expr.range);
        }

        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::RawData;
        expr.resolved_type = types.getPointer(types.getConst(types.voidType()));
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_fn_ptr") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::FunctionPointer) {
        if (state.unchecked_depth == 0) {
            return unexpected_result<const Type*>(
                "fn_ptr() is only allowed in unchecked blocks", expr.range);
        }
        if (call.arguments.size() != 1) {
            return unexpected_result<const Type*>(
                "fn_ptr() expects exactly one argument", expr.range);
        }

        auto* target_name =
            std::get_if<ast::NameExpr>(&call.arguments[0]->node);
        if (target_name == nullptr) {
            return unexpected_result<const Type*>(
                "fn_ptr() requires a function name argument", expr.range);
        }

        ast::FunctionDecl* target_function = nullptr;
        if (auto* direct_function = findVisibleFunction(target_name->name);
            direct_function != nullptr) {
            if (!call.explicit_type_arguments.empty()) {
                return unexpected_result<const Type*>(
                    "fn_ptr() explicit type arguments require a generic "
                    "function target",
                    expr.range);
            }
            target_function = direct_function;
        } else if (auto* template_decl =
                       findVisibleFunctionTemplate(target_name->name);
                   template_decl != nullptr) {
            if (call.explicit_type_arguments.size() !=
                template_decl->type_parameters.size()) {
                return unexpected_result<const Type*>(
                    "wrong number of explicit type arguments for '" +
                        target_name->name + "'",
                    expr.range);
            }

            TypeBindings type_bindings;
            for (std::size_t index = 0;
                 index < call.explicit_type_arguments.size(); ++index) {
                auto resolved_type =
                    resolveType(*call.explicit_type_arguments[index]);
                if (!resolved_type) {
                    return std::unexpected(resolved_type.error());
                }
                type_bindings.emplace(template_decl->type_parameters[index],
                                      *resolved_type);
            }

            auto instantiated =
                instantiateFunctionTemplate(*template_decl, type_bindings);
            if (!instantiated) {
                return std::unexpected(instantiated.error());
            }
            target_function = *instantiated;
        } else {
            return unexpected_result<const Type*>(
                "fn_ptr() requires a visible function target", expr.range);
        }

        if (target_function != nullptr &&
            target_function->intrinsic_lowering.has_value()) {
            return unexpected_result<const Type*>(
                "fn_ptr() cannot target intrinsic-lowered functions",
                expr.range);
        }

        bind_builtin_decl();
        call.builtin_target_function = target_function;
        call.builtin_kind = ast::BuiltinCallKind::FunctionPointer;
        expr.resolved_type = types.getPointer(types.voidType());
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_subslice") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::Subslice) {
        if (call.arguments.size() != 3) {
            return unexpected_result<const Type*>(
                "subslice() expects exactly three arguments", expr.range);
        }

        auto source_type = analyzeExpr(state, *call.arguments[0]);
        if (!source_type) {
            return std::unexpected(source_type.error());
        }

        auto ensure_shared_slice_source =
            [&](const ast::ResolvedPlace& source_place)
            -> std::expected<void, Diagnostic> {
            std::optional<std::size_t> ignored_local =
                source_place.owner_local_id;
            if (!ignored_local.has_value()) {
                if (is_borrow_like_type(*source_type)) {
                    auto source_local_id =
                        borrowSourceLocalId(state, *call.arguments[0]);
                    if (!source_local_id) {
                        return std::unexpected(source_local_id.error());
                    }
                    ignored_local = *source_local_id;
                } else if (typeContainsViews(*source_type)) {
                    auto bindings =
                        collectExprViewBindings(state, *call.arguments[0]);
                    if (!bindings) {
                        return std::unexpected(bindings.error());
                    }
                    const auto it = std::ranges::find_if(
                        *bindings, [](const ViewLeafBinding& binding) {
                            return binding.path.empty();
                        });
                    if (it != bindings->end()) {
                        ignored_local = it->source_local_id;
                    }
                }
            }
            return ensureCanBorrow(state, source_place, false,
                                   call.arguments[0]->range, ignored_local);
        };

        const auto* source_base = types.unqualify(*source_type);
        const Type* element_type = nullptr;
        if (source_base->kind == TypeKind::Array) {
            element_type = source_base->element_type;
            if ((*source_type)->is_const && !element_type->is_const) {
                element_type = types.getConst(element_type);
            }
            if (call.arguments[0]->resolved_place.has_value()) {
                expr.slice_source_place = *call.arguments[0]->resolved_place;
                auto borrow =
                    ensure_shared_slice_source(*expr.slice_source_place);
                if (!borrow) {
                    return std::unexpected(borrow.error());
                }
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
            auto borrow = ensure_shared_slice_source(*expr.slice_source_place);
            if (!borrow) {
                return std::unexpected(borrow.error());
            }
        } else if (source_base->kind == TypeKind::Slice) {
            element_type = source_base->element_type;
            auto source_place = sliceSourcePlace(state, *call.arguments[0]);
            if (!source_place) {
                return std::unexpected(source_place.error());
            }
            expr.slice_source_place = *source_place;
            if (expr.slice_source_place.has_value()) {
                auto borrow =
                    ensure_shared_slice_source(*expr.slice_source_place);
                if (!borrow) {
                    return std::unexpected(borrow.error());
                }
            }
        } else {
            return unexpected_result<const Type*>(
                "subslice() requires an array, array borrow, or slice source",
                call.arguments[0]->range);
        }

        for (std::size_t index = 1; index < call.arguments.size(); ++index) {
            auto argument_type = requireReadable(state, *call.arguments[index]);
            if (!argument_type) {
                return std::unexpected(argument_type.error());
            }
            if (!types.isInteger(*argument_type)) {
                return unexpected_result<const Type*>(
                    "subslice() offset and length must have integer type",
                    call.arguments[index]->range);
            }
        }

        const auto* result_type = types.getSlice(element_type);
        if (expected_type != nullptr &&
            types.unqualify(expected_type)->kind == TypeKind::Slice &&
            can_consume_value_type(types, result_type, expected_type)) {
            result_type = expected_type;
        }

        bind_builtin_decl();
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
                        .source_places =
                            expr.slice_source_place.has_value()
                                ? std::vector<
                                      ast::
                                          ResolvedPlace>{*expr.slice_source_place}
                                : std::vector<ast::ResolvedPlace>{},
                        .source_local_id = binding_it->source_local_id,
                        .element_sources = binding_it->element_sources,
                        .type = result_type,
                    },
                };
            }
        }
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_atomic_load") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicLoad) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 2) {
            return unexpected_result<const Type*>(
                "atomic_load() expects exactly two arguments", expr.range);
        }
        auto pointer_type = require_atomic_pointer(*call.arguments[0], false);
        if (!pointer_type) {
            return std::unexpected(pointer_type.error());
        }
        if (!is_atomic_scalar_type((*pointer_type)->element_type)) {
            return unexpected_result<const Type*>(
                "atomic_load() requires an integer, bool, or pointer pointee "
                "type",
                call.arguments[0]->range);
        }
        auto order =
            require_atomic_order(*call.arguments[1], AtomicOrderUse::Load);
        if (!order) {
            return std::unexpected(order.error());
        }
        static_cast<void>(*order);
        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::AtomicLoad;
        expr.resolved_type = (*pointer_type)->element_type;
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_atomic_store") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicStore) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 3) {
            return unexpected_result<const Type*>(
                "atomic_store() expects exactly three arguments", expr.range);
        }
        auto pointer_type = require_atomic_pointer(*call.arguments[0], true);
        if (!pointer_type) {
            return std::unexpected(pointer_type.error());
        }
        if (!is_atomic_scalar_type((*pointer_type)->element_type)) {
            return unexpected_result<const Type*>(
                "atomic_store() requires an integer, bool, or pointer pointee "
                "type",
                call.arguments[0]->range);
        }
        auto value = consumeValue(state, *call.arguments[1],
                                  (*pointer_type)->element_type);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto order =
            require_atomic_order(*call.arguments[2], AtomicOrderUse::Store);
        if (!order) {
            return std::unexpected(order.error());
        }
        static_cast<void>(*order);
        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::AtomicStore;
        expr.resolved_type = types.voidType();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_atomic_exchange") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicExchange) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 3) {
            return unexpected_result<const Type*>(
                "atomic_exchange() expects exactly three arguments",
                expr.range);
        }
        auto pointer_type = require_atomic_pointer(*call.arguments[0], true);
        if (!pointer_type) {
            return std::unexpected(pointer_type.error());
        }
        if (!is_atomic_scalar_type((*pointer_type)->element_type)) {
            return unexpected_result<const Type*>(
                "atomic_exchange() requires an integer, bool, or pointer "
                "pointee type",
                call.arguments[0]->range);
        }
        auto value = consumeValue(state, *call.arguments[1],
                                  (*pointer_type)->element_type);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto order =
            require_atomic_order(*call.arguments[2], AtomicOrderUse::Rmw);
        if (!order) {
            return std::unexpected(order.error());
        }
        static_cast<void>(*order);
        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::AtomicExchange;
        expr.resolved_type = (*pointer_type)->element_type;
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_atomic_compare_exchange") &&
         builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicCompareExchange) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 5) {
            return unexpected_result<const Type*>(
                "atomic_compare_exchange() expects exactly five arguments",
                expr.range);
        }
        auto pointer_type = require_atomic_pointer(*call.arguments[0], true);
        if (!pointer_type) {
            return std::unexpected(pointer_type.error());
        }
        if (!is_atomic_scalar_type((*pointer_type)->element_type)) {
            return unexpected_result<const Type*>(
                "atomic_compare_exchange() requires an integer, bool, or "
                "pointer pointee type",
                call.arguments[0]->range);
        }
        auto expected = consumeValue(state, *call.arguments[1],
                                     (*pointer_type)->element_type);
        if (!expected) {
            return std::unexpected(expected.error());
        }
        auto desired = consumeValue(state, *call.arguments[2],
                                    (*pointer_type)->element_type);
        if (!desired) {
            return std::unexpected(desired.error());
        }
        auto success_order = require_atomic_order(
            *call.arguments[3], AtomicOrderUse::CompareExchangeSuccess);
        if (!success_order) {
            return std::unexpected(success_order.error());
        }
        auto failure_order = require_atomic_order(
            *call.arguments[4], AtomicOrderUse::CompareExchangeFailure);
        if (!failure_order) {
            return std::unexpected(failure_order.error());
        }
        auto order_pair_valid = validate_compare_exchange_orders(
            *success_order, *failure_order, expr.range);
        if (!order_pair_valid) {
            return std::unexpected(order_pair_valid.error());
        }
        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::AtomicCompareExchange;
        expr.resolved_type = (*pointer_type)->element_type;
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_atomic_fetch_add" ||
          call.callee == "__builtin_atomic_fetch_sub" ||
          call.callee == "__builtin_atomic_fetch_and" ||
          call.callee == "__builtin_atomic_fetch_or" ||
          call.callee == "__builtin_atomic_fetch_xor") &&
         builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicFetchAdd ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicFetchSub ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicFetchAnd ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicFetchOr ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicFetchXor) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 3) {
            return unexpected_result<const Type*>(
                std::string(builtin_decl != nullptr ? builtin_decl->name
                                                    : "atomic_fetch") +
                    "() expects exactly three arguments",
                expr.range);
        }
        auto pointer_type = require_atomic_pointer(*call.arguments[0], true);
        if (!pointer_type) {
            return std::unexpected(pointer_type.error());
        }
        if (!types.isInteger((*pointer_type)->element_type)) {
            return unexpected_result<const Type*>(
                std::string(builtin_decl != nullptr ? builtin_decl->name
                                                    : "atomic_fetch") +
                    "() requires an integer pointee type",
                call.arguments[0]->range);
        }
        auto value = consumeValue(state, *call.arguments[1],
                                  (*pointer_type)->element_type);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto order =
            require_atomic_order(*call.arguments[2], AtomicOrderUse::Rmw);
        if (!order) {
            return std::unexpected(order.error());
        }
        static_cast<void>(*order);
        bind_builtin_decl();
        if (builtin_decl_kind != ast::BuiltinCallKind::None) {
            call.builtin_kind = builtin_decl_kind;
        } else if (call.callee == "__builtin_atomic_fetch_add") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicFetchAdd;
        } else if (call.callee == "__builtin_atomic_fetch_sub") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicFetchSub;
        } else if (call.callee == "__builtin_atomic_fetch_and") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicFetchAnd;
        } else if (call.callee == "__builtin_atomic_fetch_or") {
            call.builtin_kind = ast::BuiltinCallKind::AtomicFetchOr;
        } else {
            call.builtin_kind = ast::BuiltinCallKind::AtomicFetchXor;
        }
        expr.resolved_type = (*pointer_type)->element_type;
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (((call.callee == "__builtin_atomic_fence") && builtin_available) ||
        builtin_decl_kind == ast::BuiltinCallKind::AtomicFence) {
        if (!call.explicit_type_arguments.empty() &&
            !builtin_decl_is_template_instance) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (call.arguments.size() != 1) {
            return unexpected_result<const Type*>(
                "atomic_fence() expects exactly one argument", expr.range);
        }
        auto order =
            require_atomic_order(*call.arguments[0], AtomicOrderUse::Fence);
        if (!order) {
            return std::unexpected(order.error());
        }
        static_cast<void>(*order);
        bind_builtin_decl();
        call.builtin_kind = ast::BuiltinCallKind::AtomicFence;
        expr.resolved_type = types.voidType();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (function == nullptr) {
        function = findVisibleFunction(call.callee);
        if (function != nullptr && !call.explicit_type_arguments.empty()) {
            return unexpected_result<const Type*>(
                "explicit type arguments require a generic function",
                expr.range);
        }
        if (function == nullptr) {
            if (auto* template_decl = findVisibleFunctionTemplate(call.callee);
                template_decl != nullptr) {
                TypeBindings type_bindings;
                if (!call.explicit_type_arguments.empty()) {
                    if (call.explicit_type_arguments.size() !=
                        template_decl->type_parameters.size()) {
                        return unexpected_result<const Type*>(
                            "wrong number of explicit type arguments for '" +
                                call.callee + "'",
                            expr.range);
                    }
                    for (std::size_t index = 0;
                         index < call.explicit_type_arguments.size(); ++index) {
                        auto resolved_type =
                            resolveType(*call.explicit_type_arguments[index]);
                        if (!resolved_type) {
                            return std::unexpected(resolved_type.error());
                        }
                        type_bindings.emplace(
                            template_decl->type_parameters[index],
                            *resolved_type);
                    }
                } else {
                    std::vector<ast::Expr*> arguments;
                    arguments.reserve(call.arguments.size());
                    for (const auto& argument : call.arguments) {
                        arguments.push_back(argument.get());
                    }
                    auto inferred_type_bindings =
                        inferTypeBindings(state, *template_decl, arguments);
                    if (!inferred_type_bindings) {
                        return std::unexpected(inferred_type_bindings.error());
                    }
                    type_bindings = std::move(*inferred_type_bindings);
                }
                auto instantiated =
                    instantiateFunctionTemplate(*template_decl, type_bindings);
                if (!instantiated) {
                    return std::unexpected(instantiated.error());
                }
                function = *instantiated;
            } else if (!call.explicit_type_arguments.empty()) {
                return unexpected_result<const Type*>(
                    "explicit type arguments require a generic function",
                    expr.range);
            }
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
        if ((*receiver_type)->kind == TypeKind::Interface) {
            auto slot =
                types.interfaceCallableSlot(*receiver_type, interface_decl);
            if (slot.has_value()) {
                call.dispatched_interface = interface_decl;
                call.dispatched_interface_slot =
                    static_cast<std::uint32_t>(*slot);
                auto analyzed_receiver =
                    analyze_argument(receiver_argument, *receiver_type);
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
        }
        if (types.sameIgnoringTopLevelConst(*receiver_type, interface_type)) {
            call.dispatched_interface = interface_decl;
            call.dispatched_interface_slot = 0;
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
            expected_type->enum_decl != nullptr && !callee_is_qualified) {
            const auto expected_variant =
                find_variant_index(*expected_type->enum_decl, call.callee);
            if (!expected_variant.has_value()) {
                return unexpected_result<const Type*>(
                    "unknown enum variant '" + call.callee + "'", expr.range);
            }
            enum_decl = const_cast<ast::EnumDecl*>(expected_type->enum_decl);
            variant_index = *expected_variant;
        } else if (const auto variant = findVisibleEnumVariant(call.callee);
                   variant.has_value()) {
            enum_decl = variant->first;
            variant_index = variant->second;
        } else if (const auto template_variants =
                       findVisibleTemplateEnumVariants(call.callee);
                   !template_variants.empty()) {
            ast::EnumDecl* selected_template = nullptr;
            std::size_t selected_variant_index = 0;
            TypeBindings selected_bindings;

            for (const auto& [candidate_decl, candidate_variant_index] :
                 template_variants) {
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
                        .source_places = binding.source_places,
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
    auto ensured_signature = ensureFunctionSignature(resolved_function);
    if (!ensured_signature) {
        return std::unexpected(ensured_signature.error());
    }
    if (resolved_function.signature_status ==
        ast::FunctionDecl::SignatureStatus::Invalid) {
        expr.resolved_type = resolved_function.resolved_return_type != nullptr
                                 ? resolved_function.resolved_return_type
                                 : types.voidType();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
    if (resolved_function.analysis_failed) {
        expr.resolved_type = resolved_function.resolved_return_type != nullptr
                                 ? resolved_function.resolved_return_type
                                 : types.voidType();
        expr.resolved_place.reset();
        return expr.resolved_type;
    }
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
    std::vector<bool> include_projected_argument_bindings(call.arguments.size(),
                                                          false);
    for (const auto& dependency : resolved_function.return_dependencies) {
        if (!dependency.source.parameter_index.has_value() ||
            dependency.source.resolved_path.empty()) {
            continue;
        }
        include_projected_argument_bindings[*dependency.source
                                                 .parameter_index] = true;
    }

    auto build_dependency_bindings =
        [&](const ast::ReturnDependency& dependency, bool include_target_prefix)
        -> std::expected<ViewLeafBinding, Diagnostic> {
        if (!dependency.source.parameter_index.has_value()) {
            return unexpected_result<ViewLeafBinding>(
                "dependency source is missing a tracked borrow or slice leaf",
                expr.range);
        }

        const auto& source_bindings =
            dependency_argument_bindings.at(*dependency.source.parameter_index);
        return buildLeafDependencyBinding(dependency, source_bindings,
                                          include_target_prefix, expr.range);
    };

    if (!resolved_function.return_dependencies.empty()) {
        for (const auto& dependency : resolved_function.return_dependencies) {
            if (!dependency.source.parameter_index.has_value()) {
                continue;
            }
            const auto parameter_index = *dependency.source.parameter_index;
            if (dependency_argument_bindings.contains(parameter_index)) {
                continue;
            }
            auto analyzed_dependency_argument = analyzeExpr(
                state, *call.arguments[parameter_index],
                resolved_function.parameters[parameter_index].resolved_type);
            if (!analyzed_dependency_argument) {
                return std::unexpected(analyzed_dependency_argument.error());
            }
            auto bindings = collectExprViewBindings(
                state, *call.arguments[parameter_index]);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            if (include_projected_argument_bindings[parameter_index]) {
                auto extended = extendBindingsWithProjectedPointee(
                    state, *bindings,
                    resolved_function.parameters[parameter_index].resolved_type,
                    true, expr.range);
                if (!extended) {
                    return std::unexpected(extended.error());
                }
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

    for (std::size_t index = 0; index < call.arguments.size(); ++index) {
        const auto* parameter_type =
            resolved_function.parameters[index].resolved_type;
        if (parameter_type == nullptr ||
            parameter_type->kind != TypeKind::Borrow ||
            !parameter_type->is_mut ||
            parameter_type->element_type == nullptr ||
            !typeContainsViews(parameter_type->element_type)) {
            continue;
        }

        auto argument_place = borrow_argument_place(*call.arguments[index]);
        if (!argument_place) {
            return std::unexpected(argument_place.error());
        }
        invalidate_view_subtree(*argument_place, parameter_type->element_type);
    }

    for (const auto& dependency : resolved_function.return_dependencies) {
        if (dependency.target.is_return ||
            !dependency.target.parameter_index.has_value()) {
            continue;
        }

        auto argument_place = borrow_argument_place(
            *call.arguments[*dependency.target.parameter_index]);
        if (!argument_place) {
            return std::unexpected(argument_place.error());
        }
        argument_place->fields.insert(argument_place->fields.end(),
                                      dependency.target.resolved_path.begin(),
                                      dependency.target.resolved_path.end());

        auto bindings = build_dependency_bindings(dependency, false);
        if (!bindings) {
            return std::unexpected(bindings.error());
        }
        std::vector<ViewLeafBinding> assigned_bindings;
        assigned_bindings.push_back(std::move(*bindings));
        auto assigned = setAggregateViewSlots(state, *argument_place,
                                              dependency.target.resolved_type,
                                              assigned_bindings, expr.range);
        if (!assigned) {
            return std::unexpected(assigned.error());
        }
    }

    expr.resolved_type = resolved_function.resolved_return_type;
    const auto has_return_dependencies =
        std::ranges::any_of(resolved_function.return_dependencies,
                            [](const ast::ReturnDependency& dependency) {
                                return dependency.target.is_return;
                            });
    if (typeContainsViews(expr.resolved_type) && has_return_dependencies) {
        std::vector<ast::CachedViewBinding> cached_bindings;

        for (const auto& dependency : resolved_function.return_dependencies) {
            if (!dependency.target.is_return) {
                continue;
            }
            auto bindings = build_dependency_bindings(dependency, true);
            if (!bindings) {
                return std::unexpected(bindings.error());
            }
            cached_bindings.push_back(ast::CachedViewBinding{
                .path = std::move(bindings->path),
                .source_places = std::move(bindings->source_places),
                .source_local_id = bindings->source_local_id,
                .element_sources = std::move(bindings->element_sources),
                .type = bindings->type,
            });
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
                auto source_place = placeSetRepresentative(it->source_places);
                if (!source_place.has_value()) {
                    return unexpected_result<const Type*>(
                        "borrow-returning call has no tracked source",
                        expr.range);
                }
                expr.resolved_place = *source_place;
                if (expr.resolved_type->is_mut &&
                    it->source_local_id.has_value()) {
                    expr.resolved_place->owner_local_id = it->source_local_id;
                }
                expr.slice_source_place.reset();
            } else {
                expr.slice_source_place =
                    placeSetRepresentative(it->source_places);
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
                base_place.owner_local_id = *source_local_id;
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

} // namespace cyan

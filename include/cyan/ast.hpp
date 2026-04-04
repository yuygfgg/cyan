#pragma once

#include "cyan/diagnostic.hpp"
#include "cyan/source.hpp"
#include "cyan/type.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cyan::ast {

enum class UnaryOp : std::uint8_t {
    Negate,
    LogicalNot,
    BitwiseNot,
    Dereference,
    Borrow,
    BorrowMut,
    Move,
};

enum class BinaryOp : std::uint8_t {
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    ShiftLeft,
    ShiftRight,
    BitwiseAnd,
    BitwiseXor,
    BitwiseOr,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    NotEqual,
    LogicalAnd,
    LogicalOr,
};

enum class CastKind : std::uint8_t {
    None,
    Numeric,
    Const,
    Pointer,
    OwnerBorrow,
};

enum class ThreadPropertyKind : std::uint8_t {
    Local,
    Send,
    Share,
};

struct TypeSyntax;
struct Expr;
struct Stmt;
struct Block;
struct EnumDecl;
struct InterfaceDecl;
struct Module;

enum class MatchKind : std::uint8_t {
    Borrow,
    BorrowMut,
    Move,
};

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using TypeSyntaxPtr = std::unique_ptr<TypeSyntax>;
using BlockPtr = std::unique_ptr<Block>;

struct TypeSyntax {
    enum class Kind : std::uint8_t {
        Named,
        Borrow,
        Slice,
        Pointer,
        Array,
    };

    SourceRange range;
    Kind kind = Kind::Named;
    std::string name;
    SourceRange name_range;
    TypeSyntaxPtr element_type;
    std::vector<TypeSyntaxPtr> type_arguments;
    bool is_mut = false;
    bool is_shared = false;
    bool is_const = false;
    std::uint64_t array_size = 0;
    const Type* resolved_type = nullptr;
};

struct InterfaceExprTerm {
    std::string name;
    SourceRange name_range;
    bool is_negative = false;
};

struct DependencyPathSegment {
    std::string name;
    SourceRange range;
};

struct DependencyPath {
    bool is_return = false;
    std::string root_name;
    SourceRange root_range;
    std::vector<DependencyPathSegment> segments;
    SourceRange range;
    std::optional<std::size_t> parameter_index;
    std::vector<std::uint32_t> resolved_path;
    const Type* resolved_type = nullptr;
};

struct ReturnDependency {
    DependencyPath target;
    DependencyPath source;
    SourceRange range;
};

struct ResolvedPlace {
    std::size_t root_id = 0;
    bool is_external = false;
    std::optional<std::size_t> owner_local_id;
    std::vector<std::uint32_t> fields;

    auto operator==(const ResolvedPlace&) const -> bool = default;
};

struct CachedViewBinding {
    std::vector<std::uint32_t> path;
    std::vector<ResolvedPlace> source_places;
    std::optional<std::size_t> source_local_id;
    std::optional<std::size_t> owner_local_id;
    std::vector<ResolvedPlace> element_sources;
    const Type* type = nullptr;
};

struct IntegerLiteralExpr {
    std::string text;
};

struct FloatLiteralExpr {
    double value = 0.0;
};

struct CharLiteralExpr {
    char value = '\0';
};

struct BoolLiteralExpr {
    bool value = false;
};

struct StringLiteralExpr {
    std::string value;
};

struct NameExpr {
    std::string name;
    SourceRange name_range;
    std::size_t local_id = 0;
    const FunctionDecl* function = nullptr;
};

struct UnaryExpr {
    UnaryOp op = UnaryOp::Negate;
    ExprPtr operand;
};

struct BinaryExpr {
    BinaryOp op = BinaryOp::Add;
    ExprPtr lhs;
    ExprPtr rhs;
};

enum class BuiltinCallKind : std::uint8_t {
    None,
    Len,
    Subslice,
    RawData,
    FromRawParts,
    FromRawPartsOn,
    FunctionPointer,
    Panic,
    ResultTry,
    AtomicRelaxedOrder,
    AtomicAcquireOrder,
    AtomicReleaseOrder,
    AtomicAcqRelOrder,
    AtomicSeqCstOrder,
    AtomicLoad,
    AtomicStore,
    AtomicExchange,
    AtomicCompareExchange,
    AtomicFetchAdd,
    AtomicFetchSub,
    AtomicFetchAnd,
    AtomicFetchOr,
    AtomicFetchXor,
    AtomicFence,
};

struct CallExpr {
    std::string callee;
    SourceRange callee_range;
    std::vector<TypeSyntaxPtr> explicit_type_arguments;
    std::vector<ExprPtr> arguments;
    BuiltinCallKind builtin_kind = BuiltinCallKind::None;
    const FunctionDecl* function = nullptr;
    const FunctionDecl* builtin_target_function = nullptr;
    const EnumDecl* enum_decl = nullptr;
    const InterfaceDecl* dispatched_interface = nullptr;
    std::uint32_t dispatched_interface_slot = 0;
    std::uint32_t variant_index = 0;
};

struct MemberExpr {
    ExprPtr base;
    std::string field_name;
    SourceRange field_range;
    std::uint32_t field_index = 0;
};

struct IndexExpr {
    ExprPtr base;
    ExprPtr index;
};

struct InitListExpr {
    std::vector<ExprPtr> elements;
};

struct ArrayLiteralExpr {
    std::vector<ExprPtr> elements;
};

struct CastExpr {
    ExprPtr operand;
    TypeSyntaxPtr target_type;
    ExprPtr owner_expr;
    CastKind cast_kind = CastKind::None;
};

struct SizeofExpr {
    TypeSyntaxPtr type;
    const Type* operand_type = nullptr;
};

struct Expr {
    using Variant =
        std::variant<IntegerLiteralExpr, FloatLiteralExpr, CharLiteralExpr,
                     BoolLiteralExpr, StringLiteralExpr, NameExpr, UnaryExpr,
                     BinaryExpr, CallExpr, MemberExpr, IndexExpr, InitListExpr,
                     ArrayLiteralExpr, CastExpr, SizeofExpr>;

    SourceRange range;
    Variant node;
    const Type* resolved_type = nullptr;
    std::optional<ResolvedPlace> resolved_place;
    const Type* interface_source_type = nullptr;
    std::vector<const FunctionDecl*> interface_impls;
    const Type* slice_source_type = nullptr;
    std::optional<ResolvedPlace> slice_source_place;
    std::size_t slice_storage_local_id = 0;
    std::optional<std::vector<CachedViewBinding>> cached_view_bindings;
    std::vector<std::size_t> early_return_drop_local_ids;
};

struct VarDeclStmt {
    TypeSyntaxPtr type;
    std::string name;
    SourceRange name_range;
    ExprPtr initializer;
    std::size_t local_id = 0;
};

struct ExprStmt {
    ExprPtr expr;
};

struct AssignStmt {
    ExprPtr target;
    ExprPtr value;
    bool drop_old_value = false;
};

struct UpdateStmt {
    ExprPtr target;
    bool is_increment = true;
};

struct ReturnStmt {
    ExprPtr value;
    std::vector<std::size_t> drop_local_ids;
};

struct DropStmt {
    ExprPtr value;
};

struct IfStmt {
    ExprPtr condition;
    BlockPtr then_block;
    BlockPtr else_block;
};

struct WhileStmt {
    ExprPtr condition;
    BlockPtr body;
};

struct BreakStmt {
    std::vector<std::size_t> drop_local_ids;
};

struct ContinueStmt {
    std::vector<std::size_t> drop_local_ids;
};

struct ForStmt {
    StmtPtr initializer;
    ExprPtr condition;
    StmtPtr step;
    BlockPtr body;
    std::vector<std::size_t> exit_drop_local_ids;
};

struct UncheckedStmt {
    BlockPtr body;
};

struct SwitchCase {
    SourceRange range;
    bool is_default = false;
    std::string variant_name;
    SourceRange variant_name_range;
    std::optional<std::string> binding_name;
    std::optional<SourceRange> binding_name_range;
    BlockPtr body;
    std::uint32_t variant_index = 0;
    std::size_t binding_local_id = 0;
    const Type* binding_type = nullptr;
};

struct SwitchStmt {
    ExprPtr scrutinee;
    MatchKind match_kind = MatchKind::Borrow;
    std::vector<SwitchCase> cases;
    const EnumDecl* enum_decl = nullptr;
};

struct Block {
    SourceRange range;
    std::vector<StmtPtr> statements;
    std::vector<std::size_t> exit_drop_local_ids;
};

struct Stmt {
    using Variant =
        std::variant<VarDeclStmt, ExprStmt, AssignStmt, ReturnStmt, UpdateStmt,
                     DropStmt, IfStmt, WhileStmt, ForStmt, BreakStmt,
                     ContinueStmt, UncheckedStmt, SwitchStmt, Block>;

    SourceRange range;
    Variant node;
};

struct StructField {
    TypeSyntaxPtr type;
    std::string name;
    SourceRange name_range;
    SourceRange range;
    const Type* resolved_type = nullptr;
};

enum class ImplTargetKind : std::uint8_t {
    None,
    Named,
};

struct StructDecl {
    SourceRange range;
    std::string name;
    SourceRange name_range;
    std::string linkage_name;
    std::vector<std::string> type_parameters;
    std::vector<SourceRange> type_parameter_ranges;
    std::vector<StructField> fields;
    const Type* resolved_type = nullptr;
    const StructDecl* template_decl = nullptr;
    std::vector<const Type*> type_arguments;
    bool is_export = false;
    const Module* owner_module = nullptr;
};

struct EnumVariant {
    SourceRange range;
    std::string name;
    SourceRange name_range;
    TypeSyntaxPtr payload_type;
    const Type* resolved_type = nullptr;
    std::uint32_t tag = 0;
};

struct EnumDecl {
    SourceRange range;
    std::string name;
    SourceRange name_range;
    std::string linkage_name;
    std::vector<std::string> type_parameters;
    std::vector<SourceRange> type_parameter_ranges;
    std::vector<EnumVariant> variants;
    const Type* resolved_type = nullptr;
    const EnumDecl* template_decl = nullptr;
    std::vector<const Type*> type_arguments;
    bool is_export = false;
    const Module* owner_module = nullptr;
};

struct Parameter {
    TypeSyntaxPtr type;
    std::string name;
    SourceRange name_range;
    SourceRange range;
    const Type* resolved_type = nullptr;
    std::size_t local_id = 0;
};

struct InterfaceDecl {
    SourceRange range;
    std::string receiver_type_parameter;
    SourceRange receiver_type_parameter_range;
    TypeSyntaxPtr return_type;
    std::string name;
    SourceRange name_range;
    std::vector<Parameter> parameters;
    const Type* resolved_return_type = nullptr;
    bool receiver_is_mut = false;
    bool is_unchecked = false;
    bool is_export = false;
    const Module* owner_module = nullptr;
};

struct InterfaceAliasDecl {
    SourceRange range;
    std::string name;
    SourceRange name_range;
    std::vector<InterfaceExprTerm> terms;
    const Type* resolved_type = nullptr;
    bool is_export = false;
    const Module* owner_module = nullptr;
};

struct IntrinsicLoweringArgument {
    std::size_t index = 0;
    std::string value_spec;
};

struct ExternSymbolCase {
    SourceRange range;
    std::optional<std::string> return_type_spec;
    std::vector<std::pair<std::size_t, std::string>> argument_type_specs;
    std::string symbol_name;
};

enum class LoweringConstantKind : std::uint8_t {
    None,
    NullValue,
};

struct IntrinsicLowering {
    SourceRange range;
    std::string intrinsic_name;
    LoweringConstantKind constant_kind = LoweringConstantKind::None;
    std::optional<std::string> intrinsic_return_spec;
    std::optional<std::size_t> return_argument_index;
    std::vector<IntrinsicLoweringArgument> argument_overrides;
    std::vector<ExternSymbolCase> intrinsic_cases;
};

struct BuiltinLowering {
    SourceRange range;
    BuiltinCallKind builtin_kind = BuiltinCallKind::None;
    bool allow_untracked_view_return = false;
};

struct FunctionDecl {
    enum class SignatureStatus : std::uint8_t {
        Unchecked,
        Valid,
        Invalid,
    };

    SourceRange range;
    TypeSyntaxPtr return_type;
    std::string name;
    SourceRange name_range;
    std::string linkage_name;
    std::vector<std::string> type_parameters;
    std::vector<SourceRange> type_parameter_ranges;
    std::vector<Parameter> parameters;
    BlockPtr body;
    std::vector<std::size_t> exit_drop_local_ids;
    const Type* resolved_return_type = nullptr;
    // After semantic validation this is normalized to leaf-level mappings.
    std::vector<ReturnDependency> return_dependencies;
    std::vector<ReturnDependency> declared_return_dependencies;
    const FunctionDecl* template_decl = nullptr;
    std::vector<const Type*> type_arguments;
    ImplTargetKind impl_target_kind = ImplTargetKind::None;
    std::string impl_target_name;
    const InterfaceDecl* interface_decl = nullptr;
    std::optional<IntrinsicLowering> intrinsic_lowering;
    std::optional<BuiltinLowering> builtin_lowering;
    std::vector<ExternSymbolCase> extern_symbol_cases;
    std::string resolved_extern_symbol;
    SignatureStatus signature_status = SignatureStatus::Unchecked;
    bool analysis_failed = false;
    bool is_unchecked = false;
    bool is_export = false;
    bool is_extern = false;
    const Module* owner_module = nullptr;
};

struct PropertyImplDecl {
    SourceRange range;
    ThreadPropertyKind property_kind = ThreadPropertyKind::Local;
    SourceRange property_name_range;
    std::vector<std::string> type_parameters;
    std::vector<SourceRange> type_parameter_ranges;
    TypeSyntaxPtr target_type;
    std::string impl_target_name;
    const Module* impl_target_module = nullptr;
    bool is_unchecked = false;
    const Module* owner_module = nullptr;
};

struct ImportDecl {
    SourceRange range;
    bool is_export = false;
    bool is_builtin = false;
    std::size_t parent_depth = 0;
    std::string module_name;
    SourceRange module_name_range;
    std::vector<SourceRange> module_name_part_ranges;
    std::optional<std::string> alias;
    std::optional<SourceRange> alias_range;
    Module* imported_module = nullptr;
};

using Decl = std::variant<StructDecl, EnumDecl, InterfaceDecl,
                          InterfaceAliasDecl, FunctionDecl, PropertyImplDecl>;

struct Module {
    const SourceFile* source = nullptr;
    std::filesystem::path path;
    std::string module_name;
    bool is_builtin = false;
    std::vector<ImportDecl> imports;
    std::vector<Decl> declarations;
    DiagnosticList diagnostics;
};

struct Package {
    Module* entry_module = nullptr;
    std::filesystem::path root_dir;
    std::vector<std::unique_ptr<SourceFile>> sources;
    std::vector<std::unique_ptr<Module>> modules;
    std::vector<std::unique_ptr<Decl>> instantiated_declarations;
};

} // namespace cyan::ast

namespace cyan {

using StructDecl = ast::StructDecl;
using FunctionDecl = ast::FunctionDecl;

} // namespace cyan

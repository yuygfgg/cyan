#pragma once

#include "cyan/ast.hpp"
#include "cyan/diagnostic.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyan {

struct SemanticScope {
    std::unordered_map<std::string, const ast::Module*> import_namespaces;
    std::unordered_map<std::string, const ast::StructDecl*> structs;
    std::unordered_map<std::string, const ast::StructDecl*> struct_templates;
    std::unordered_map<std::string, const ast::EnumDecl*> enums;
    std::unordered_map<std::string, const ast::EnumDecl*> enum_templates;
    std::unordered_map<std::string, const ast::InterfaceDecl*> interfaces;
    std::unordered_map<std::string, const ast::FunctionDecl*> functions;
    std::unordered_map<std::string, const ast::FunctionDecl*>
        function_templates;
    std::unordered_map<std::string, const ast::FunctionDecl*> interface_impls;
    std::unordered_map<std::string, const ast::FunctionDecl*>
        interface_impl_templates;
    std::unordered_map<std::string,
                       std::pair<const ast::EnumDecl*, std::size_t>>
        variants;
    std::unordered_map<
        std::string, std::vector<std::pair<const ast::EnumDecl*, std::size_t>>>
        template_variants;
};

struct SemanticLocalSymbol {
    enum class Kind : std::uint8_t {
        Var,
        Parameter,
        SwitchBinding,
    };

    Kind kind = Kind::Var;
    std::string name;
    SourceRange declaration_range;
    SourceRange name_range;
    const Type* type = nullptr;
    const ast::FunctionDecl* owner_function = nullptr;
    const ast::VarDeclStmt* var_decl = nullptr;
    const ast::Parameter* parameter = nullptr;
    const ast::SwitchCase* switch_case = nullptr;
};

struct SemanticAnalysis {
    DiagnosticList diagnostics;
    std::unordered_map<const ast::Module*, SemanticScope> local_scopes;
    std::unordered_map<const ast::Module*, SemanticScope> visible_scopes;
    std::unordered_map<std::size_t, SemanticLocalSymbol> local_symbols;

    [[nodiscard]] auto localScopeFor(const ast::Module& module) const
        -> const SemanticScope* {
        if (const auto it = local_scopes.find(&module);
            it != local_scopes.end()) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] auto visibleScopeFor(const ast::Module& module) const
        -> const SemanticScope* {
        if (const auto it = visible_scopes.find(&module);
            it != visible_scopes.end()) {
            return &it->second;
        }
        return nullptr;
    }

    [[nodiscard]] auto localSymbol(std::size_t local_id) const
        -> const SemanticLocalSymbol* {
        if (const auto it = local_symbols.find(local_id);
            it != local_symbols.end()) {
            return &it->second;
        }
        return nullptr;
    }
};

class SemanticAnalyzer {
  public:
    explicit SemanticAnalyzer(TypeContext& types);

    auto analyze(ast::Package& package) -> SemanticAnalysis;

  private:
    struct ModuleScope {
        std::unordered_map<std::string, ast::Module*> import_namespaces;
        std::unordered_map<std::string, ast::StructDecl*> structs;
        std::unordered_map<std::string, ast::StructDecl*> struct_templates;
        std::unordered_map<std::string, ast::EnumDecl*> enums;
        std::unordered_map<std::string, ast::EnumDecl*> enum_templates;
        std::unordered_map<std::string, ast::InterfaceDecl*> interfaces;
        std::unordered_map<std::string, ast::FunctionDecl*> functions;
        std::unordered_map<std::string, ast::FunctionDecl*> function_templates;
        std::unordered_map<std::string, ast::FunctionDecl*> interface_impls;
        std::unordered_map<std::string, ast::FunctionDecl*>
            interface_impl_templates;
        std::unordered_map<std::string, std::pair<ast::EnumDecl*, std::size_t>>
            variants;
        std::unordered_map<std::string,
                           std::vector<std::pair<ast::EnumDecl*, std::size_t>>>
            template_variants;
    };

    struct LocalState {
        enum class Status : std::uint8_t {
            Uninitialized,
            Live,
            Moved,
            MaybeLive,
            Unavailable,
        };

        std::string name;
        const Type* type = nullptr;
        bool in_scope = true;
        std::size_t scope_depth = 0;
        std::size_t unique_id = 0;
        Status status = Status::Uninitialized;
        std::vector<ast::ResolvedPlace> borrow_origins;
        std::vector<ast::ResolvedPlace> element_origins;
        std::optional<std::size_t> reborrow_parent_local_id;
        bool is_parameter = false;
        bool is_hidden = false;
        bool is_view_slot = false;
        bool slot_root_is_external = false;
        std::size_t slot_root_id = 0;
        std::vector<std::uint32_t> slot_path;
    };

    struct TemporaryLoan {
        ast::ResolvedPlace place;
        bool is_mut = false;
    };

    struct ScopeFrame {
        std::unordered_map<std::string, std::size_t> locals;
    };

    struct ContinueState {
        std::vector<LocalState> locals;
        std::vector<ScopeFrame> scopes;
        std::unordered_map<std::string, std::size_t> view_slot_locals;
    };

    struct LoopState {
        std::vector<LocalState> entry_locals;
        std::size_t body_scope_depth = 0;
        std::vector<ContinueState> continue_states;
    };

    struct FunctionState {
        const ast::FunctionDecl* function = nullptr;
        const Type* return_type = nullptr;
        std::vector<ScopeFrame> scopes;
        std::vector<LocalState> locals;
        std::unordered_map<std::string, std::size_t> view_slot_locals;
        std::vector<TemporaryLoan> temporary_loans;
        std::vector<std::size_t> temporary_suspended_local_ids;
        std::vector<LoopState> loops;
        bool reachable = true;
        std::size_t unchecked_depth = 0;
        std::size_t relaxed_place_resolution_depth = 0;
    };

    struct ViewLeafInfo {
        std::vector<std::uint32_t> path;
        const Type* type = nullptr;
    };

    struct ViewLeafBinding {
        std::vector<std::uint32_t> path;
        std::vector<ast::ResolvedPlace> source_places;
        std::optional<std::size_t> source_local_id;
        std::vector<ast::ResolvedPlace> element_sources;
        const Type* type = nullptr;
    };

    struct ViewShape {
        const Type* type = nullptr;
        std::vector<ViewLeafInfo> leaves;
    };

    enum class MergePolicy : std::uint8_t {
        Join,
        Exact,
    };

    using TypeBindings = std::unordered_map<std::string, const Type*>;
    using ExportedSymbolMap =
        std::unordered_map<std::size_t, SemanticLocalSymbol>;

    [[nodiscard]] auto exportScope(const ModuleScope& scope) const
        -> SemanticScope;
    auto recordLocalSymbol(ExportedSymbolMap& symbols,
                           const ast::FunctionDecl& owner,
                           const ast::VarDeclStmt& stmt,
                           SourceRange declaration_range) const -> void;
    auto recordLocalSymbol(ExportedSymbolMap& symbols,
                           const ast::FunctionDecl& owner,
                           const ast::Parameter& parameter) const -> void;
    auto recordLocalSymbol(ExportedSymbolMap& symbols,
                           const ast::FunctionDecl& owner,
                           const ast::SwitchCase& switch_case,
                           SourceRange declaration_range) const -> void;
    auto collectStmtLocalSymbols(ExportedSymbolMap& symbols,
                                 const ast::FunctionDecl& owner,
                                 const ast::Stmt& stmt) const -> void;
    auto collectBlockLocalSymbols(ExportedSymbolMap& symbols,
                                  const ast::FunctionDecl& owner,
                                  const ast::Block& block) const -> void;
    [[nodiscard]] auto buildAnalysis() const -> SemanticAnalysis;

    auto collectDeclarations(ast::Package& package)
        -> std::expected<void, Diagnostic>;
    auto buildVisibleScopes(ast::Package& package)
        -> std::expected<void, Diagnostic>;
    auto buildExportScope(const ast::Module& module)
        -> std::expected<const ModuleScope*, Diagnostic>;
    auto buildVisibleScope(const ast::Module& module)
        -> std::expected<const ModuleScope*, Diagnostic>;
    auto registerImplDeclarations(ast::Package& package)
        -> std::expected<void, Diagnostic>;
    auto analyzeStruct(ast::StructDecl& decl)
        -> std::expected<void, Diagnostic>;
    auto analyzeInterface(ast::InterfaceDecl& decl)
        -> std::expected<void, Diagnostic>;
    auto analyzeFunction(ast::FunctionDecl& decl)
        -> std::expected<void, Diagnostic>;
    auto analyzeBlock(FunctionState& state, ast::Block& block)
        -> std::expected<void, Diagnostic>;
    auto analyzeStmt(FunctionState& state, ast::Stmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeVarDecl(FunctionState& state, ast::VarDeclStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeAssign(FunctionState& state, ast::AssignStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeUpdate(FunctionState& state, ast::UpdateStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeReturn(FunctionState& state, ast::ReturnStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeDrop(FunctionState& state, ast::DropStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeIf(FunctionState& state, ast::IfStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeWhile(FunctionState& state, ast::WhileStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeFor(FunctionState& state, ast::ForStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeBreak(FunctionState& state, ast::BreakStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeContinue(FunctionState& state, ast::ContinueStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeUnchecked(FunctionState& state, ast::UncheckedStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeSwitch(FunctionState& state, ast::SwitchStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeExprStmt(FunctionState& state, ast::ExprStmt& stmt)
        -> std::expected<void, Diagnostic>;
    auto analyzeExpr(FunctionState& state, ast::Expr& expr,
                     const Type* expected_type = nullptr)
        -> std::expected<const Type*, Diagnostic>;
    auto analyzeCall(FunctionState& state, ast::Expr& expr, ast::CallExpr& call,
                     const Type* expected_type)
        -> std::expected<const Type*, Diagnostic>;
    auto analyzeMember(FunctionState& state, ast::Expr& expr,
                       ast::MemberExpr& member)
        -> std::expected<const Type*, Diagnostic>;
    [[nodiscard]] auto typeContainsViews(const Type* type) const -> bool;
    [[nodiscard]] auto viewShape(const Type* type) const -> const ViewShape&;
    [[nodiscard]] auto collectViewLeafInfos(const Type* type) const
        -> const std::vector<ViewLeafInfo>&;
    auto resolveDependencyPath(ast::DependencyPath& path, const Type* root_type,
                               std::optional<std::size_t> parameter_index,
                               bool allow_borrow_projection = false)
        -> std::expected<void, Diagnostic>;
    auto validateReturnDependencies(ast::FunctionDecl& decl)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto
    findViewBinding(std::vector<ViewLeafBinding>& bindings,
                    const std::vector<std::uint32_t>& path) const
        -> ViewLeafBinding*;
    [[nodiscard]] auto
    findViewBinding(const std::vector<ViewLeafBinding>& bindings,
                    const std::vector<std::uint32_t>& path) const
        -> const ViewLeafBinding*;
    [[nodiscard]] auto bindingPlaces(const ViewLeafBinding& binding) const
        -> const std::vector<ast::ResolvedPlace>*;
    auto extendBindingsWithProjectedPointee(
        FunctionState& state, std::vector<ViewLeafBinding>& bindings,
        const Type* parameter_type, bool clear_mut_source_locals,
        SourceRange range) -> std::expected<void, Diagnostic>;
    auto buildLeafDependencyBinding(
        const ast::ReturnDependency& dependency,
        const std::vector<ViewLeafBinding>& source_bindings,
        bool include_target_prefix, SourceRange range) const
        -> std::expected<ViewLeafBinding, Diagnostic>;
    [[nodiscard]] auto viewSlotKey(bool is_external, std::size_t root_id,
                                   const std::vector<std::uint32_t>& path) const
        -> std::string;
    auto registerViewSlot(FunctionState& state, std::size_t local_index)
        -> void;
    auto findViewSlotLocal(FunctionState& state, bool is_external,
                           std::size_t root_id,
                           const std::vector<std::uint32_t>& path) const
        -> std::optional<std::size_t>;
    auto declareAggregateViewSlots(FunctionState& state,
                                   const ast::ResolvedPlace& base_place,
                                   const Type* type, bool abstract_sources,
                                   LocalState::Status status)
        -> std::expected<void, Diagnostic>;
    auto ensureAggregateViewSlots(FunctionState& state,
                                  const ast::ResolvedPlace& base_place,
                                  const Type* type)
        -> std::expected<void, Diagnostic>;
    auto ensureViewSubtreeLive(FunctionState& state,
                               const ast::ResolvedPlace& base_place,
                               const Type* type, SourceRange range)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto viewStatusMessage(LocalState::Status status) const
        -> std::string;
    auto collectSlotBindings(FunctionState& state,
                             const ast::ResolvedPlace& base_place,
                             const Type* type)
        -> std::expected<std::vector<ViewLeafBinding>, Diagnostic>;
    auto collectProjectedViewBindings(
        FunctionState& state,
        const std::vector<ast::ResolvedPlace>& base_places, const Type* type)
        -> std::expected<std::vector<ViewLeafBinding>, Diagnostic>;
    auto collectExprViewBindings(FunctionState& state, ast::Expr& expr)
        -> std::expected<std::vector<ViewLeafBinding>, Diagnostic>;
    auto setAggregateViewSlots(FunctionState& state,
                               const ast::ResolvedPlace& target_place,
                               const Type* target_type,
                               const std::vector<ViewLeafBinding>& bindings,
                               SourceRange range)
        -> std::expected<void, Diagnostic>;
    auto markAggregateViewSlots(FunctionState& state,
                                const ast::ResolvedPlace& base_place,
                                const Type* type, LocalState::Status status)
        -> void;
    auto ensureViewSourceLive(FunctionState& state, const LocalState& local,
                              SourceRange range)
        -> std::expected<void, Diagnostic>;
    auto ensureViewSourceOutlivesLocal(FunctionState& state,
                                       const LocalState& local,
                                       SourceRange range)
        -> std::expected<void, Diagnostic>;
    auto validateMutableParameterDependencies(FunctionState& state,
                                              SourceRange range)
        -> std::expected<void, Diagnostic>;
    auto resolveType(ast::TypeSyntax& type)
        -> std::expected<const Type*, Diagnostic>;
    auto
    instantiateStructTemplate(ast::StructDecl& decl,
                              const std::vector<const Type*>& type_arguments)
        -> std::expected<ast::StructDecl*, Diagnostic>;
    auto instantiateEnumTemplate(ast::EnumDecl& decl,
                                 const std::vector<const Type*>& type_arguments)
        -> std::expected<ast::EnumDecl*, Diagnostic>;
    auto instantiateFunctionTemplate(ast::FunctionDecl& decl,
                                     const TypeBindings& type_bindings)
        -> std::expected<ast::FunctionDecl*, Diagnostic>;
    auto appendInstantiatedDecl(ast::Decl decl) -> ast::Decl*;
    auto registerVisibleDecl(ModuleScope& scope, ast::Decl& decl,
                             SourceRange conflict_range)
        -> std::expected<void, Diagnostic>;
    auto registerImportNamespace(ModuleScope& scope, std::string name,
                                 ast::Module* module,
                                 SourceRange conflict_range)
        -> std::expected<void, Diagnostic>;
    auto mergeImportedScope(ModuleScope& target, const ModuleScope& source,
                            SourceRange conflict_range)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto visibleScopeFor(const ast::Module& module) const
        -> const ModuleScope&;
    [[nodiscard]] auto localScopeFor(const ast::Module& module) const
        -> const ModuleScope&;
    [[nodiscard]] auto exportScopeFor(const ast::Module& module) const
        -> const ModuleScope&;
    [[nodiscard]] auto findNamedTypeInModule(const ast::Module& module,
                                             std::string_view name) const
        -> const Type*;
    [[nodiscard]] auto findStructTemplateInModule(const ast::Module& module,
                                                  std::string_view name) const
        -> ast::StructDecl*;
    [[nodiscard]] auto findEnumTemplateInModule(const ast::Module& module,
                                                std::string_view name) const
        -> ast::EnumDecl*;
    [[nodiscard]] auto findInterfaceInModule(const ast::Module& module,
                                             std::string_view name) const
        -> const ast::InterfaceDecl*;
    [[nodiscard]] auto findFunctionInModule(const ast::Module& module,
                                            std::string_view name) const
        -> ast::FunctionDecl*;
    [[nodiscard]] auto findFunctionTemplateInModule(const ast::Module& module,
                                                    std::string_view name) const
        -> ast::FunctionDecl*;
    [[nodiscard]] auto findEnumVariantInModule(const ast::Module& module,
                                               std::string_view name) const
        -> std::optional<std::pair<ast::EnumDecl*, std::size_t>>;
    [[nodiscard]] auto
    findTemplateEnumVariantsInModule(const ast::Module& module,
                                     std::string_view name) const
        -> std::vector<std::pair<ast::EnumDecl*, std::size_t>>;
    [[nodiscard]] auto findVisibleNamedType(std::string_view name) const
        -> const Type*;
    [[nodiscard]] auto findVisibleStructTemplate(std::string_view name) const
        -> ast::StructDecl*;
    [[nodiscard]] auto findVisibleEnumTemplate(std::string_view name) const
        -> ast::EnumDecl*;
    auto inferTypeBindings(FunctionState& state, const ast::FunctionDecl& decl,
                           const std::vector<ast::Expr*>& arguments)
        -> std::expected<TypeBindings, Diagnostic>;
    auto matchTypePattern(const ast::TypeSyntax& pattern,
                          const ast::Module& owner_module,
                          const std::vector<std::string>& type_parameters,
                          const Type* actual_type, TypeBindings& type_bindings)
        -> std::expected<void, Diagnostic>;
    auto validateResolvedImplSignature(ast::FunctionDecl& decl,
                                       const Type* target_type)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto findVisibleInterface(std::string_view name) const
        -> const ast::InterfaceDecl*;
    [[nodiscard]] auto findVisibleFunction(std::string_view name) const
        -> ast::FunctionDecl*;
    [[nodiscard]] auto findVisibleFunctionTemplate(std::string_view name) const
        -> ast::FunctionDecl*;
    [[nodiscard]] auto findVisibleEnumVariant(std::string_view name) const
        -> std::optional<std::pair<ast::EnumDecl*, std::size_t>>;
    [[nodiscard]] auto
    findVisibleTemplateEnumVariants(std::string_view name) const
        -> std::vector<std::pair<ast::EnumDecl*, std::size_t>>;
    auto validateExternSignature(const ast::FunctionDecl& decl)
        -> std::expected<void, Diagnostic>;
    auto findImplForType(const ast::InterfaceDecl& interface_decl,
                         const Type* receiver_type)
        -> std::expected<ast::FunctionDecl*, Diagnostic>;
    auto ensureDropImplForType(const Type* type)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto implReceiverPattern(const ast::FunctionDecl& decl) const
        -> const ast::TypeSyntax*;
    auto interfaceReceiverType(const Type* argument_type) const -> const Type*;
    auto coerceExprToInterface(FunctionState& state, ast::Expr& expr,
                               const Type* interface_type)
        -> std::expected<void, Diagnostic>;
    auto coerceExprToSlice(FunctionState& state, ast::Expr& expr,
                           const Type* slice_type)
        -> std::expected<void, Diagnostic>;
    auto borrowSourcePlace(FunctionState& state, ast::Expr& expr)
        -> std::expected<ast::ResolvedPlace, Diagnostic>;
    auto sliceSourcePlace(FunctionState& state, ast::Expr& expr)
        -> std::expected<std::optional<ast::ResolvedPlace>, Diagnostic>;
    auto projectedPlaceSources(FunctionState& state, ast::Expr& expr)
        -> std::expected<std::vector<ast::ResolvedPlace>, Diagnostic>;
    auto borrowSourceLocalId(FunctionState& state, ast::Expr& expr)
        -> std::expected<std::optional<std::size_t>, Diagnostic>;
    auto analyzeEnum(ast::EnumDecl& decl) -> std::expected<void, Diagnostic>;
    auto declareLocal(FunctionState& state, std::string name, const Type* type,
                      bool is_parameter, SourceRange range)
        -> std::expected<std::size_t, Diagnostic>;
    auto declareHiddenLocal(FunctionState& state, const Type* type)
        -> std::size_t;
    auto lookupLocal(FunctionState& state, std::string_view name) const
        -> std::optional<std::size_t>;
    auto findLocalById(FunctionState& state, std::size_t local_id) const
        -> std::optional<std::size_t>;
    [[nodiscard]] auto findLocalById(const FunctionState& state,
                                     std::size_t local_id) const
        -> std::optional<std::size_t>;
    auto requireReadable(FunctionState& state, ast::Expr& expr,
                         const Type* expected_type = nullptr)
        -> std::expected<const Type*, Diagnostic>;
    auto consumeValue(FunctionState& state, ast::Expr& expr,
                      const Type* expected_type)
        -> std::expected<const Type*, Diagnostic>;
    auto createNamedBorrow(FunctionState& state, std::size_t local_id,
                           ast::Expr& initializer, const Type* target_type)
        -> std::expected<void, Diagnostic>;
    auto assignNamedBorrow(FunctionState& state, std::size_t local_id,
                           ast::Expr& value) -> std::expected<void, Diagnostic>;
    auto ensureBorrowSourceType(FunctionState& state, ast::Expr& expr,
                                const Type* target_type)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto
    placeSetRepresentative(const std::vector<ast::ResolvedPlace>& places) const
        -> std::optional<ast::ResolvedPlace>;
    [[nodiscard]] auto
    canonicalizePlaces(std::vector<ast::ResolvedPlace> places) const
        -> std::vector<ast::ResolvedPlace>;
    [[nodiscard]] auto
    joinPlaces(const std::vector<ast::ResolvedPlace>& lhs,
               const std::vector<ast::ResolvedPlace>& rhs) const
        -> std::vector<ast::ResolvedPlace>;
    [[nodiscard]] auto topLevelOrigins(const LocalState& local) const
        -> std::vector<ast::ResolvedPlace>;
    auto setTopLevelOrigins(LocalState& local,
                            std::vector<ast::ResolvedPlace> origins) const
        -> void;
    [[nodiscard]] auto isDefinitelyLive(LocalState::Status status) const
        -> bool;
    [[nodiscard]] auto mayBeLive(LocalState::Status status) const -> bool;
    [[nodiscard]] auto joinLocalStatus(LocalState::Status lhs,
                                       LocalState::Status rhs) const
        -> LocalState::Status;
    [[nodiscard]] auto canJoinLocalStatus(const LocalState& local) const
        -> bool;
    [[nodiscard]] auto localStatusMessage(const LocalState& local) const
        -> std::string;
    auto borrowFromExpr(FunctionState& state, ast::Expr& expr, bool want_mut,
                        bool temporary_only)
        -> std::expected<ast::ResolvedPlace, Diagnostic>;
    auto resolvePlace(FunctionState& state, ast::Expr& expr)
        -> std::expected<ast::ResolvedPlace, Diagnostic>;
    auto projectedPlaceTargets(FunctionState& state, ast::Expr& expr)
        -> std::expected<std::vector<ast::ResolvedPlace>, Diagnostic>;
    auto ensureCanRead(FunctionState& state, const ast::ResolvedPlace& place,
                       SourceRange range,
                       std::optional<std::size_t> ignored_local = std::nullopt)
        -> std::expected<void, Diagnostic>;
    auto ensureCanWrite(FunctionState& state, const ast::ResolvedPlace& place,
                        SourceRange range,
                        std::optional<std::size_t> ignored_local = std::nullopt)
        -> std::expected<void, Diagnostic>;
    auto
    ensureCanBorrow(FunctionState& state, const ast::ResolvedPlace& place,
                    bool is_mut, SourceRange range,
                    std::optional<std::size_t> ignored_local = std::nullopt)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto sharedReborrowParentLocalIds(const FunctionState& state,
                                                    std::size_t local_id) const
        -> std::vector<std::size_t>;
    [[nodiscard]] auto loanOwnerMatchesIgnored(
        const FunctionState& state, std::optional<std::size_t> owner_local_id,
        std::optional<std::size_t> ignored_local_id) const -> bool;
    [[nodiscard]] auto placesOverlap(const ast::ResolvedPlace& lhs,
                                     const ast::ResolvedPlace& rhs) const
        -> bool;
    [[nodiscard]] auto localPlace(std::size_t local_id) const
        -> ast::ResolvedPlace;
    auto branchMerge(FunctionState& into, const FunctionState& then_state,
                     const FunctionState& else_state, SourceRange range,
                     MergePolicy policy = MergePolicy::Join)
        -> std::expected<void, Diagnostic>;
    auto mergeReachableStates(FunctionState& into,
                              const std::vector<FunctionState>& states,
                              SourceRange range)
        -> std::expected<void, Diagnostic>;
    auto appendContinueStates(FunctionState& into, const FunctionState& from)
        -> void;
    auto requireLoopBreakState(const FunctionState& state, SourceRange range)
        -> std::expected<void, Diagnostic>;
    auto clearStatementTemporaries(FunctionState& state) -> void;
    auto enterScope(FunctionState& state) -> void;
    auto leaveScope(FunctionState& state) -> void;
    auto releaseReborrowParent(FunctionState& state, LocalState& local) -> void;
    auto attachReborrowParent(FunctionState& state, LocalState& local,
                              ast::Expr& source_expr)
        -> std::expected<void, Diagnostic>;
    [[nodiscard]] auto activeNamedLoans(const FunctionState& state) const
        -> std::vector<TemporaryLoan>;
    [[nodiscard]] auto collectDropLocalIds(
        const FunctionState& state,
        const std::function<bool(const LocalState&)>& predicate) const
        -> std::vector<std::size_t>;
    auto report(Diagnostic diagnostic) -> void;

    ast::Package* current_package = nullptr;
    const ast::Module* active_module = nullptr;
    TypeContext& types;
    DiagnosticList diagnostics;
    std::unordered_map<const ast::Module*, ModuleScope> local_scopes;
    std::unordered_map<const ast::Module*, ModuleScope> export_scopes;
    std::unordered_map<const ast::Module*, ModuleScope> visible_scopes;
    std::unordered_map<const ast::Module*, bool> building_export_scopes;
    std::unordered_map<const ast::Module*, bool> building_visible_scopes;
    std::unordered_map<std::string, std::vector<ast::FunctionDecl*>>
        package_impls;
    mutable std::unordered_map<const Type*, ViewShape> view_shapes;
    std::unordered_map<std::string, ast::StructDecl*> instantiated_structs;
    std::unordered_map<std::string, ast::EnumDecl*> instantiated_enums;
    std::unordered_map<std::string, ast::FunctionDecl*> instantiated_functions;
    std::size_t next_external_root = 1;
    std::size_t next_local_id = 1;
};

} // namespace cyan

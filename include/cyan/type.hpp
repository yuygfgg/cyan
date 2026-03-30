#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyan {

namespace ast {
struct StructDecl;
struct FunctionDecl;
struct EnumDecl;
struct InterfaceDecl;
} // namespace ast

enum class TypeKind : std::uint8_t {
    Void,
    Int,
    Float,
    Char,
    Bool,
    Array,
    Slice,
    Struct,
    Enum,
    Interface,
    Borrow,
    Pointer,
};

struct Type {
    TypeKind kind = TypeKind::Void;
    const Type* element_type = nullptr;
    const ast::StructDecl* struct_decl = nullptr;
    const ast::EnumDecl* enum_decl = nullptr;
    const ast::InterfaceDecl* interface_decl = nullptr;
    bool is_mut = false;
    bool is_const = false;
    std::uint64_t array_size = 0;
    std::string name;
    std::string linkage_name;
};

class TypeContext {
  public:
    TypeContext();

    [[nodiscard]] auto voidType() const -> const Type*;
    [[nodiscard]] auto intType() const -> const Type*;
    [[nodiscard]] auto floatType() const -> const Type*;
    [[nodiscard]] auto charType() const -> const Type*;
    [[nodiscard]] auto boolType() const -> const Type*;

    auto registerStruct(std::string name, std::string linkage_name,
                        const ast::StructDecl* decl) -> const Type*;
    auto registerEnum(std::string name, std::string linkage_name,
                      const ast::EnumDecl* decl) -> const Type*;
    auto getInterface(const ast::InterfaceDecl* decl) -> const Type*;
    [[nodiscard]] auto findNamed(std::string_view name) const -> const Type*;
    auto getConst(const Type* type) -> const Type*;
    auto getBorrow(const Type* pointee, bool is_mut) -> const Type*;
    auto getPointer(const Type* pointee) -> const Type*;
    auto getArray(const Type* element, std::uint64_t size) -> const Type*;
    auto getSlice(const Type* element) -> const Type*;
    auto registerDropFunction(const Type* type,
                              const ast::FunctionDecl* function) -> void;
    [[nodiscard]] auto dropFunction(const Type* type) const
        -> const ast::FunctionDecl*;

    [[nodiscard]] auto unqualify(const Type* type) const -> const Type*;
    [[nodiscard]] auto isSame(const Type* lhs, const Type* rhs) const -> bool;
    [[nodiscard]] auto sameIgnoringTopLevelConst(const Type* lhs,
                                                 const Type* rhs) const -> bool;
    [[nodiscard]] auto sameIgnoringConst(const Type* lhs, const Type* rhs) const
        -> bool;
    [[nodiscard]] auto isCopy(const Type* type) const -> bool;
    [[nodiscard]] auto needsDrop(const Type* type) const -> bool;
    [[nodiscard]] auto describe(const Type* type) const -> std::string;

  private:
    auto makeType(Type type) -> const Type*;
    auto borrowKey(const Type* pointee, bool is_mut) const -> std::string;
    auto pointerKey(const Type* pointee) const -> std::string;
    auto arrayKey(const Type* element, std::uint64_t size) const -> std::string;
    auto sliceKey(const Type* element) const -> std::string;

    std::vector<std::unique_ptr<Type>> owned_types;
    std::unordered_map<std::string, const Type*> named_types;
    std::unordered_map<std::string, const Type*> borrow_types;
    std::unordered_map<std::string, const Type*> pointer_types;
    std::unordered_map<std::string, const Type*> array_types;
    std::unordered_map<std::string, const Type*> slice_types;
    std::unordered_map<const Type*, const Type*> const_types;
    std::unordered_map<const Type*, const Type*> unqualified_types;
    std::unordered_map<const ast::InterfaceDecl*, const Type*> interface_types;
    std::unordered_map<const Type*, const ast::FunctionDecl*> drop_functions;
    const Type* void_builtin_type = nullptr;
    const Type* int_builtin_type = nullptr;
    const Type* float_builtin_type = nullptr;
    const Type* char_builtin_type = nullptr;
    const Type* bool_builtin_type = nullptr;
};

} // namespace cyan

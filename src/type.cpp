#include "cyan/type.hpp"

#include "cyan/ast.hpp"

#include <algorithm>
#include <sstream>

namespace cyan {

TypeContext::TypeContext() {
    const auto register_builtin = [&](TypeKind kind, std::string name,
                                      std::uint16_t bit_width = 0,
                                      bool is_signed = false) -> const Type* {
        const auto linkage_name = name;
        const auto* type = makeType(Type{.kind = kind,
                                         .element_type = nullptr,
                                         .struct_decl = nullptr,
                                         .enum_decl = nullptr,
                                         .interface_decl = nullptr,
                                         .bit_width = bit_width,
                                         .is_signed = is_signed,
                                         .is_mut = false,
                                         .is_const = false,
                                         .array_size = 0,
                                         .name = std::move(name),
                                         .linkage_name = linkage_name});
        named_types.emplace(type->name, type);
        return type;
    };

    void_builtin_type = register_builtin(TypeKind::Void, "void");
    i8_builtin_type = register_builtin(TypeKind::Integer, "i8", 8, true);
    i16_builtin_type = register_builtin(TypeKind::Integer, "i16", 16, true);
    i32_builtin_type = register_builtin(TypeKind::Integer, "i32", 32, true);
    i64_builtin_type = register_builtin(TypeKind::Integer, "i64", 64, true);
    u8_builtin_type = register_builtin(TypeKind::Integer, "u8", 8, false);
    u16_builtin_type = register_builtin(TypeKind::Integer, "u16", 16, false);
    u32_builtin_type = register_builtin(TypeKind::Integer, "u32", 32, false);
    u64_builtin_type = register_builtin(TypeKind::Integer, "u64", 64, false);
    f32_builtin_type = register_builtin(TypeKind::Float, "f32", 32);
    f64_builtin_type = register_builtin(TypeKind::Float, "f64", 64);
    char_builtin_type = register_builtin(TypeKind::Char, "char", 8);
    bool_builtin_type = register_builtin(TypeKind::Bool, "bool", 1);

    default_integer_builtin_type = i64_builtin_type;
    default_float_builtin_type = f64_builtin_type;
}

auto TypeContext::voidType() const -> const Type* { return void_builtin_type; }

auto TypeContext::defaultIntegerType() const -> const Type* {
    return default_integer_builtin_type;
}

auto TypeContext::defaultFloatType() const -> const Type* {
    return default_float_builtin_type;
}

auto TypeContext::i8Type() const -> const Type* { return i8_builtin_type; }

auto TypeContext::i16Type() const -> const Type* { return i16_builtin_type; }

auto TypeContext::i32Type() const -> const Type* { return i32_builtin_type; }

auto TypeContext::i64Type() const -> const Type* { return i64_builtin_type; }

auto TypeContext::u8Type() const -> const Type* { return u8_builtin_type; }

auto TypeContext::u16Type() const -> const Type* { return u16_builtin_type; }

auto TypeContext::u32Type() const -> const Type* { return u32_builtin_type; }

auto TypeContext::u64Type() const -> const Type* { return u64_builtin_type; }

auto TypeContext::f32Type() const -> const Type* { return f32_builtin_type; }

auto TypeContext::f64Type() const -> const Type* { return f64_builtin_type; }

auto TypeContext::integerType(std::uint16_t bit_width, bool is_signed) const
    -> const Type* {
    switch (bit_width) {
    case 8:
        return is_signed ? i8_builtin_type : u8_builtin_type;
    case 16:
        return is_signed ? i16_builtin_type : u16_builtin_type;
    case 32:
        return is_signed ? i32_builtin_type : u32_builtin_type;
    case 64:
        return is_signed ? i64_builtin_type : u64_builtin_type;
    default:
        return nullptr;
    }
}

auto TypeContext::floatType(std::uint16_t bit_width) const -> const Type* {
    switch (bit_width) {
    case 32:
        return f32_builtin_type;
    case 64:
        return f64_builtin_type;
    default:
        return nullptr;
    }
}

auto TypeContext::charType() const -> const Type* { return char_builtin_type; }

auto TypeContext::boolType() const -> const Type* { return bool_builtin_type; }

auto TypeContext::isInteger(const Type* type) const -> bool {
    return type != nullptr && unqualify(type)->kind == TypeKind::Integer;
}

auto TypeContext::isFloat(const Type* type) const -> bool {
    return type != nullptr && unqualify(type)->kind == TypeKind::Float;
}

auto TypeContext::isSignedInteger(const Type* type) const -> bool {
    type = type == nullptr ? nullptr : unqualify(type);
    return type != nullptr && type->kind == TypeKind::Integer &&
           type->is_signed;
}

auto TypeContext::isUnsignedInteger(const Type* type) const -> bool {
    type = type == nullptr ? nullptr : unqualify(type);
    return type != nullptr && type->kind == TypeKind::Integer &&
           !type->is_signed;
}

auto TypeContext::isNumeric(const Type* type) const -> bool {
    type = type == nullptr ? nullptr : unqualify(type);
    return type != nullptr &&
           (type->kind == TypeKind::Integer || type->kind == TypeKind::Float);
}

auto TypeContext::makeType(Type type) -> const Type* {
    owned_types.push_back(std::make_unique<Type>(std::move(type)));
    return owned_types.back().get();
}

auto TypeContext::registerStruct(std::string name, std::string linkage_name,
                                 const ast::StructDecl* decl) -> const Type* {
    const auto* const type =
        makeType(Type{.kind = TypeKind::Struct,
                      .element_type = nullptr,
                      .struct_decl = decl,
                      .enum_decl = nullptr,
                      .interface_decl = nullptr,
                      .bit_width = 0,
                      .is_signed = false,
                      .is_mut = false,
                      .is_const = false,
                      .array_size = 0,
                      .name = std::move(name),
                      .linkage_name = std::move(linkage_name)});
    return type;
}

auto TypeContext::registerEnum(std::string name, std::string linkage_name,
                               const ast::EnumDecl* decl) -> const Type* {
    const auto* const type =
        makeType(Type{.kind = TypeKind::Enum,
                      .element_type = nullptr,
                      .struct_decl = nullptr,
                      .enum_decl = decl,
                      .interface_decl = nullptr,
                      .bit_width = 0,
                      .is_signed = false,
                      .is_mut = false,
                      .is_const = false,
                      .array_size = 0,
                      .name = std::move(name),
                      .linkage_name = std::move(linkage_name)});
    return type;
}

auto TypeContext::getInterface(const ast::InterfaceDecl* decl) -> const Type* {
    if (const auto it = interface_types.find(decl);
        it != interface_types.end()) {
        return it->second;
    }

    const auto linkage_name = "iface$" + decl->name;
    const auto* const type = makeType(Type{.kind = TypeKind::Interface,
                                           .element_type = nullptr,
                                           .struct_decl = nullptr,
                                           .enum_decl = nullptr,
                                           .interface_decl = decl,
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = decl->receiver_is_mut,
                                           .is_const = false,
                                           .array_size = 0,
                                           .name = decl->name,
                                           .linkage_name = linkage_name});
    interface_types.emplace(decl, type);
    return type;
}

auto TypeContext::findNamed(std::string_view name) const -> const Type* {
    const auto it = named_types.find(std::string(name));
    return it == named_types.end() ? nullptr : it->second;
}

auto TypeContext::getConst(const Type* type) -> const Type* {
    if (type->is_const) {
        return type;
    }
    if (const auto it = const_types.find(type); it != const_types.end()) {
        return it->second;
    }

    auto qualified = *type;
    qualified.is_const = true;
    const auto* result = makeType(std::move(qualified));
    const_types.emplace(type, result);
    unqualified_types.emplace(result, type);
    return result;
}

auto TypeContext::borrowKey(const Type* pointee, bool is_mut) const
    -> std::string {
    return (is_mut ? "mut:" : "shr:") + describe(pointee);
}

auto TypeContext::pointerKey(const Type* pointee) const -> std::string {
    return "ptr:" + describe(pointee);
}

auto TypeContext::arrayKey(const Type* element, std::uint64_t size) const
    -> std::string {
    return "arr:" + std::to_string(size) + ":" + describe(element);
}

auto TypeContext::sliceKey(const Type* element) const -> std::string {
    return "slice:" + describe(element);
}

auto TypeContext::getBorrow(const Type* pointee, bool is_mut) -> const Type* {
    const auto key = borrowKey(pointee, is_mut);
    if (const auto it = borrow_types.find(key); it != borrow_types.end()) {
        return it->second;
    }

    const auto* const type = makeType(Type{.kind = TypeKind::Borrow,
                                           .element_type = pointee,
                                           .struct_decl = nullptr,
                                           .enum_decl = nullptr,
                                           .interface_decl = nullptr,
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = is_mut,
                                           .is_const = false,
                                           .array_size = 0,
                                           .name = key,
                                           .linkage_name = key});
    borrow_types.emplace(key, type);
    return type;
}

auto TypeContext::getPointer(const Type* pointee) -> const Type* {
    const auto key = pointerKey(pointee);
    if (const auto it = pointer_types.find(key); it != pointer_types.end()) {
        return it->second;
    }

    const auto* const type = makeType(Type{.kind = TypeKind::Pointer,
                                           .element_type = pointee,
                                           .struct_decl = nullptr,
                                           .enum_decl = nullptr,
                                           .interface_decl = nullptr,
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = false,
                                           .is_const = false,
                                           .array_size = 0,
                                           .name = key,
                                           .linkage_name = key});
    pointer_types.emplace(key, type);
    return type;
}

auto TypeContext::getArray(const Type* element, std::uint64_t size)
    -> const Type* {
    const auto key = arrayKey(element, size);
    if (const auto it = array_types.find(key); it != array_types.end()) {
        return it->second;
    }

    std::ostringstream name;
    name << describe(element) << '[' << size << ']';
    const auto* const type = makeType(Type{.kind = TypeKind::Array,
                                           .element_type = element,
                                           .struct_decl = nullptr,
                                           .enum_decl = nullptr,
                                           .interface_decl = nullptr,
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = false,
                                           .is_const = false,
                                           .array_size = size,
                                           .name = name.str(),
                                           .linkage_name = key});
    array_types.emplace(key, type);
    return type;
}

auto TypeContext::getSlice(const Type* element) -> const Type* {
    const auto key = sliceKey(element);
    if (const auto it = slice_types.find(key); it != slice_types.end()) {
        return it->second;
    }

    const auto* const type = makeType(Type{.kind = TypeKind::Slice,
                                           .element_type = element,
                                           .struct_decl = nullptr,
                                           .enum_decl = nullptr,
                                           .interface_decl = nullptr,
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = false,
                                           .is_const = false,
                                           .array_size = 0,
                                           .name = "[]" + describe(element),
                                           .linkage_name = key});
    slice_types.emplace(key, type);
    return type;
}

auto TypeContext::registerDropFunction(const Type* type,
                                       const ast::FunctionDecl* function)
    -> void {
    drop_functions[unqualify(type)] = function;
}

auto TypeContext::dropFunction(const Type* type) const
    -> const ast::FunctionDecl* {
    type = unqualify(type);
    if (const auto it = drop_functions.find(type); it != drop_functions.end()) {
        return it->second;
    }
    return nullptr;
}

auto TypeContext::unqualify(const Type* type) const -> const Type* {
    if (!type->is_const) {
        return type;
    }
    if (const auto it = unqualified_types.find(type);
        it != unqualified_types.end()) {
        return it->second;
    }
    return type;
}

auto TypeContext::isSame(const Type* lhs, const Type* rhs) const -> bool {
    return lhs == rhs;
}

auto TypeContext::sameIgnoringTopLevelConst(const Type* lhs,
                                            const Type* rhs) const -> bool {
    return unqualify(lhs) == unqualify(rhs);
}

auto TypeContext::sameIgnoringConst(const Type* lhs, const Type* rhs) const
    -> bool {
    lhs = unqualify(lhs);
    rhs = unqualify(rhs);
    if (lhs == rhs) {
        return true;
    }
    if (lhs->kind != rhs->kind || lhs->is_mut != rhs->is_mut ||
        lhs->array_size != rhs->array_size ||
        lhs->bit_width != rhs->bit_width || lhs->is_signed != rhs->is_signed) {
        return false;
    }

    switch (lhs->kind) {
    case TypeKind::Void:
    case TypeKind::Integer:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
        return true;
    case TypeKind::Struct:
        return lhs->struct_decl == rhs->struct_decl;
    case TypeKind::Enum:
        return lhs->enum_decl == rhs->enum_decl;
    case TypeKind::Interface:
        return lhs->interface_decl == rhs->interface_decl;
    case TypeKind::Borrow:
    case TypeKind::Pointer:
    case TypeKind::Slice:
    case TypeKind::Array:
        return sameIgnoringConst(lhs->element_type, rhs->element_type);
    }
    return false;
}

auto TypeContext::isCopy(const Type* type) const -> bool {
    type = unqualify(type);
    switch (type->kind) {
    case TypeKind::Void:
        return true;
    case TypeKind::Integer:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
    case TypeKind::Pointer:
        return true;
    case TypeKind::Borrow:
        return !type->is_mut;
    case TypeKind::Slice:
        return true;
    case TypeKind::Array:
        return isCopy(type->element_type);
    case TypeKind::Struct: {
        if (drop_functions.contains(type)) {
            return false;
        }
        const auto& decl = *type->struct_decl;
        return std::ranges::all_of(decl.fields, [this](const auto& field) {
            return isCopy(field.resolved_type);
        });
    }
    case TypeKind::Enum: {
        if (drop_functions.contains(type)) {
            return false;
        }
        const auto& decl = *type->enum_decl;
        return std::ranges::all_of(decl.variants, [this](const auto& variant) {
            return variant.resolved_type == nullptr ||
                   isCopy(variant.resolved_type);
        });
    }
    case TypeKind::Interface:
        return !type->is_mut;
    }
    return false;
}

auto TypeContext::needsDrop(const Type* type) const -> bool {
    type = unqualify(type);
    if (type->kind == TypeKind::Borrow || type->kind == TypeKind::Interface ||
        type->kind == TypeKind::Slice) {
        return false;
    }
    if (drop_functions.contains(type)) {
        return true;
    }
    return !isCopy(type);
}

auto TypeContext::describe(const Type* type) const -> std::string {
    if (type->is_const) {
        const auto* unqualified = unqualify(type);
        const auto inner = describe(unqualified);
        if (unqualified->kind == TypeKind::Pointer ||
            unqualified->kind == TypeKind::Borrow ||
            unqualified->kind == TypeKind::Slice ||
            unqualified->kind == TypeKind::Array) {
            return "const (" + inner + ')';
        }
        return "const " + inner;
    }

    switch (type->kind) {
    case TypeKind::Void:
    case TypeKind::Integer:
    case TypeKind::Float:
    case TypeKind::Char:
    case TypeKind::Bool:
        return type->name;
    case TypeKind::Array: {
        std::ostringstream stream;
        stream << describe(type->element_type) << '[' << type->array_size
               << ']';
        return stream.str();
    }
    case TypeKind::Slice:
        return "[]" + describe(type->element_type);
    case TypeKind::Struct:
    case TypeKind::Enum:
        return type->name;
    case TypeKind::Interface:
        return '&' + type->name;
    case TypeKind::Borrow: {
        std::ostringstream stream;
        stream << '&';
        if (type->is_mut) {
            stream << "mut ";
        }
        stream << describe(type->element_type);
        return stream.str();
    }
    case TypeKind::Pointer:
        return describe(type->element_type) + '*';
    }
    return "<invalid>";
}

} // namespace cyan

#include "cyan/type.hpp"

#include "cyan/ast.hpp"

#include <algorithm>
#include <ranges>
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
                                         .interface_alias_decl = nullptr,
                                         .interface_members = {},
                                         .interface_exclusions = {},
                                         .interface_markers = {},
                                         .interface_marker_exclusions = {},
                                         .bit_width = bit_width,
                                         .is_signed = is_signed,
                                         .is_mut = false,
                                         .is_shared = false,
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
    i128_builtin_type = register_builtin(TypeKind::Integer, "i128", 128, true);
    u8_builtin_type = register_builtin(TypeKind::Integer, "u8", 8, false);
    u16_builtin_type = register_builtin(TypeKind::Integer, "u16", 16, false);
    u32_builtin_type = register_builtin(TypeKind::Integer, "u32", 32, false);
    u64_builtin_type = register_builtin(TypeKind::Integer, "u64", 64, false);
    u128_builtin_type = register_builtin(TypeKind::Integer, "u128", 128, false);
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

auto TypeContext::i128Type() const -> const Type* { return i128_builtin_type; }

auto TypeContext::u8Type() const -> const Type* { return u8_builtin_type; }

auto TypeContext::u16Type() const -> const Type* { return u16_builtin_type; }

auto TypeContext::u32Type() const -> const Type* { return u32_builtin_type; }

auto TypeContext::u64Type() const -> const Type* { return u64_builtin_type; }

auto TypeContext::u128Type() const -> const Type* { return u128_builtin_type; }

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
    case 128:
        return is_signed ? i128_builtin_type : u128_builtin_type;
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
                      .interface_alias_decl = nullptr,
                      .interface_members = {},
                      .interface_exclusions = {},
                      .interface_markers = {},
                      .interface_marker_exclusions = {},
                      .bit_width = 0,
                      .is_signed = false,
                      .is_mut = false,
                      .is_shared = false,
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
                      .interface_alias_decl = nullptr,
                      .interface_members = {},
                      .interface_exclusions = {},
                      .interface_markers = {},
                      .interface_marker_exclusions = {},
                      .bit_width = 0,
                      .is_signed = false,
                      .is_mut = false,
                      .is_shared = false,
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

    const auto linkage_name =
        "iface$" +
        (decl->owner_module != nullptr ? decl->owner_module->module_name + "$"
                                       : std::string()) +
        decl->name;
    std::vector<const ast::InterfaceDecl*> interface_members{decl};
    const auto key =
        interfaceKey(interface_members, {}, {}, {}, decl->receiver_is_mut);
    const auto interface_it = interface_types_by_key.find(key);
    if (interface_it != interface_types_by_key.end()) {
        interface_types.emplace(decl, interface_it->second);
        return interface_it->second;
    }

    const auto* const type = makeType(Type{
        .kind = TypeKind::Interface,
        .element_type = nullptr,
        .struct_decl = nullptr,
        .enum_decl = nullptr,
        .interface_decl = decl,
        .interface_alias_decl = nullptr,
        .interface_members = std::move(interface_members),
        .interface_exclusions = {},
        .interface_markers = {},
        .interface_marker_exclusions = {},
        .bit_width = 0,
        .is_signed = false,
        .is_mut = decl->receiver_is_mut,
        .is_shared = false,
        .is_const = false,
        .array_size = 0,
        .name = decl->name,
        .linkage_name = linkage_name,
    });
    interface_types.emplace(decl, type);
    interface_types_by_key.emplace(key, type);
    return type;
}

auto TypeContext::getInterfaceAlias(
    std::string name, std::string linkage_name,
    const ast::InterfaceAliasDecl* decl,
    std::vector<const ast::InterfaceDecl*> interface_members,
    std::vector<const ast::InterfaceDecl*> interface_exclusions,
    std::vector<std::string> interface_markers,
    std::vector<std::string> interface_marker_exclusions, bool receiver_is_mut)
    -> const Type* {
    if (decl != nullptr) {
        if (const auto it = interface_alias_types.find(decl);
            it != interface_alias_types.end()) {
            return it->second;
        }
    }

    const auto interface_less = [](const ast::InterfaceDecl* lhs,
                                   const ast::InterfaceDecl* rhs) {
        const auto lhs_name =
            (lhs != nullptr && lhs->owner_module != nullptr
                 ? lhs->owner_module->module_name + "." + lhs->name
             : lhs != nullptr ? lhs->name
                              : std::string());
        const auto rhs_name =
            (rhs != nullptr && rhs->owner_module != nullptr
                 ? rhs->owner_module->module_name + "." + rhs->name
             : rhs != nullptr ? rhs->name
                              : std::string());
        return lhs_name < rhs_name;
    };

    std::ranges::sort(interface_members, interface_less);
    interface_members.erase(
        std::unique(interface_members.begin(), interface_members.end()),
        interface_members.end());
    std::ranges::sort(interface_exclusions, interface_less);
    interface_exclusions.erase(
        std::unique(interface_exclusions.begin(), interface_exclusions.end()),
        interface_exclusions.end());
    std::ranges::sort(interface_markers);
    interface_markers.erase(
        std::unique(interface_markers.begin(), interface_markers.end()),
        interface_markers.end());
    std::ranges::sort(interface_marker_exclusions);
    interface_marker_exclusions.erase(
        std::unique(interface_marker_exclusions.begin(),
                    interface_marker_exclusions.end()),
        interface_marker_exclusions.end());

    const auto key =
        interfaceKey(interface_members, interface_exclusions, interface_markers,
                     interface_marker_exclusions, receiver_is_mut);
    const auto has_plain_interface_shape =
        interface_members.size() == 1 && interface_exclusions.empty() &&
        interface_markers.empty() && interface_marker_exclusions.empty();

    const Type* canonical_type = nullptr;
    if (const auto it = interface_types_by_key.find(key);
        it != interface_types_by_key.end()) {
        canonical_type = it->second;
    } else {
        canonical_type = makeType(Type{
            .kind = TypeKind::Interface,
            .element_type = nullptr,
            .struct_decl = nullptr,
            .enum_decl = nullptr,
            .interface_decl = decl == nullptr && has_plain_interface_shape
                                  ? interface_members.front()
                                  : nullptr,
            .interface_alias_decl = nullptr,
            .interface_members = interface_members,
            .interface_exclusions = interface_exclusions,
            .interface_markers = interface_markers,
            .interface_marker_exclusions = interface_marker_exclusions,
            .bit_width = 0,
            .is_signed = false,
            .is_mut = receiver_is_mut,
            .is_shared = false,
            .is_const = false,
            .array_size = 0,
            .name = key,
            .linkage_name = key,
        });
        interface_types_by_key.emplace(key, canonical_type);
    }

    if (decl == nullptr) {
        return canonical_type;
    }

    auto alias_type = *canonical_type;
    alias_type.interface_decl = nullptr;
    alias_type.interface_alias_decl = decl;
    alias_type.name = std::move(name);
    alias_type.linkage_name = std::move(linkage_name);
    const auto* const type = makeType(std::move(alias_type));
    interface_alias_types.emplace(decl, type);
    return type;
}

auto TypeContext::findNamed(std::string_view name) const -> const Type* {
    const auto it = named_types.find(std::string(name));
    return it == named_types.end() ? nullptr : it->second;
}

auto TypeContext::interfaceCallableSlot(const Type* interface_type,
                                        const ast::InterfaceDecl* decl) const
    -> std::optional<std::size_t> {
    if (interface_type == nullptr) {
        return std::nullopt;
    }
    interface_type = unqualify(interface_type);
    if (interface_type->kind != TypeKind::Interface || decl == nullptr) {
        return std::nullopt;
    }
    for (std::size_t index = 0;
         index < interface_type->interface_members.size(); ++index) {
        if (interface_type->interface_members[index] == decl) {
            return index;
        }
    }
    return std::nullopt;
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

auto TypeContext::pointerKey(const Type* pointee, bool is_shared) const
    -> std::string {
    return std::string(is_shared ? "sptr:" : "ptr:") + describe(pointee);
}

auto TypeContext::arrayKey(const Type* element, std::uint64_t size) const
    -> std::string {
    return "arr:" + std::to_string(size) + ":" + describe(element);
}

auto TypeContext::sliceKey(const Type* element) const -> std::string {
    return "slice:" + describe(element);
}

auto TypeContext::interfaceKey(
    const std::vector<const ast::InterfaceDecl*>& interface_members,
    const std::vector<const ast::InterfaceDecl*>& interface_exclusions,
    const std::vector<std::string>& interface_markers,
    const std::vector<std::string>& interface_marker_exclusions,
    bool receiver_is_mut) const -> std::string {
    const auto append_interface_name = [](std::ostringstream& stream,
                                          const ast::InterfaceDecl* decl) {
        if (decl != nullptr && decl->owner_module != nullptr) {
            stream << decl->owner_module->module_name << '.';
        }
        if (decl != nullptr) {
            stream << decl->name;
        }
    };

    std::ostringstream stream;
    stream << "ifaceexpr:" << (receiver_is_mut ? "mut" : "shr");
    for (const auto* decl : interface_members) {
        stream << ":+";
        append_interface_name(stream, decl);
    }
    for (const auto* decl : interface_exclusions) {
        stream << ":-";
        append_interface_name(stream, decl);
    }
    for (const auto& marker : interface_markers) {
        stream << ":+@" << marker;
    }
    for (const auto& marker : interface_marker_exclusions) {
        stream << ":-@" << marker;
    }
    return stream.str();
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
                                           .interface_alias_decl = nullptr,
                                           .interface_members = {},
                                           .interface_exclusions = {},
                                           .interface_markers = {},
                                           .interface_marker_exclusions = {},
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = is_mut,
                                           .is_shared = false,
                                           .is_const = false,
                                           .array_size = 0,
                                           .name = key,
                                           .linkage_name = key});
    borrow_types.emplace(key, type);
    return type;
}

auto TypeContext::getPointer(const Type* pointee, bool is_shared)
    -> const Type* {
    const auto key = pointerKey(pointee, is_shared);
    if (const auto it = pointer_types.find(key); it != pointer_types.end()) {
        return it->second;
    }

    const auto* const type = makeType(Type{.kind = TypeKind::Pointer,
                                           .element_type = pointee,
                                           .struct_decl = nullptr,
                                           .enum_decl = nullptr,
                                           .interface_decl = nullptr,
                                           .interface_alias_decl = nullptr,
                                           .interface_members = {},
                                           .interface_exclusions = {},
                                           .interface_markers = {},
                                           .interface_marker_exclusions = {},
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = false,
                                           .is_shared = is_shared,
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
                                           .interface_alias_decl = nullptr,
                                           .interface_members = {},
                                           .interface_exclusions = {},
                                           .interface_markers = {},
                                           .interface_marker_exclusions = {},
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = false,
                                           .is_shared = false,
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
                                           .interface_alias_decl = nullptr,
                                           .interface_members = {},
                                           .interface_exclusions = {},
                                           .interface_markers = {},
                                           .interface_marker_exclusions = {},
                                           .bit_width = 0,
                                           .is_signed = false,
                                           .is_mut = false,
                                           .is_shared = false,
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
        lhs->is_shared != rhs->is_shared ||
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
        return lhs->interface_members == rhs->interface_members &&
               lhs->interface_exclusions == rhs->interface_exclusions &&
               lhs->interface_markers == rhs->interface_markers &&
               lhs->interface_marker_exclusions ==
                   rhs->interface_marker_exclusions;
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
        return std::string(type->is_shared ? "shared " : "") +
               describe(type->element_type) + '*';
    }
    return "<invalid>";
}

} // namespace cyan

// Shorthands for building descriptor tables in the schema tests.
#pragma once

#include <kladde/schema.hpp>

#include <string>
#include <vector>

namespace kladde::schema::test {

inline TypeRef ref(std::size_t i) { return TypeRef{i}; }

inline Field field(std::string name, std::size_t type) { return Field{std::move(name), ref(type)}; }

inline Variant variant(std::uint64_t discriminant, std::string name, std::size_t payload) {
    return Variant{discriminant, std::move(name), ref(payload)};
}

inline TypeDescriptor strukt(std::string name, std::vector<Field> fields) {
    return Struct{std::move(name), std::move(fields)};
}

/// `struct Point { x: i32, y: i32 }` over one shared `i32`.
inline TypeTable point() {
    return TypeTable({strukt("Point", {field("x", 1), field("y", 1)}), Primitive::i32});
}

/// `enum List { Nil, Cons(i32, List) }`: `Nil` holds the empty Struct, and
/// `Cons` a Struct of its two positional fields.
inline TypeTable list() {
    return TypeTable({
        Enum{"List", 4, {variant(0, "Nil", 2), variant(1, "Cons", 3)}},
        Primitive::i32,
        strukt("Unit", {}),
        strukt("Cons", {field("0", 1), field("1", 0)}),
    });
}

/// `Map { key: Text, value: u32 }`.
inline TypeTable text_to_u32() { return TypeTable({Map{ref(1), ref(2)}, Text{}, Primitive::u32}); }

inline TypeTable opaque_versioned(std::uint64_t major, std::uint64_t minor, std::uint64_t patch) {
    return TypeTable({Opaque{"lib", "T", {major, minor, patch}, 8, {}}});
}

inline std::vector<std::uint8_t> bytes(std::string const& s) { return {s.begin(), s.end()}; }

}  // namespace kladde::schema::test

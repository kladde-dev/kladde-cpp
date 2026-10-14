// Helpers the schema's translation units share; not part of the public API.
#pragma once

#include <kladde/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace kladde::schema::detail {

/// A descriptor's bytes with its references cut out: `bytes`, with each
/// slot's reference inserted at its offset. The canonical encoding writes a
/// reference as its index, the fingerprint's hash input as a token, so both
/// share this one description of what goes where, and cannot disagree on
/// canonical order.
struct Layout {
    struct Slot {
        std::size_t at;  // offset into `bytes`, nondecreasing across slots
        TypeRef ref;
    };
    std::vector<std::uint8_t> bytes;
    std::vector<Slot> slots;
};

enum class LayoutFor {
    /// The canonical encoding (spec/schema/canonical-encoding).
    encoding,
    /// The hash input of spec/schema/fingerprints: names of structs and
    /// enums left out, an Opaque's version reduced to its compatibility
    /// component.
    hashing,
};

Layout layout(TypeDescriptor const& d, LayoutFor purpose);

/// An enum's variants in canonical order: ascending discriminant, ties in
/// stored order (a table with ties fails `validate`).
std::vector<Variant const*> canonical_variants(std::vector<Variant> const& variants);

/// Every reference `d` makes, in canonical order.
std::vector<TypeRef> references(TypeDescriptor const& d);

/// Whether `bytes` is well-formed UTF-8: shortest forms, no surrogates,
/// nothing above U+10FFFF.
bool valid_utf8(std::span<std::uint8_t const> bytes);

/// Whether `code` names a primitive this implementation knows.
constexpr bool known_primitive(std::uint8_t code) { return code <= 11; }

}  // namespace kladde::schema::detail

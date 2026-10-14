// The schema: type descriptors, their canonical encoding, and fingerprints,
// as content/spec/schema describes them. Language-neutral, and independent
// of the store.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include <kladde/error.hpp>

namespace kladde::schema {

/// A reference to a descriptor: its index in the type table.
struct TypeRef {
    std::size_t index = 0;
    friend bool operator==(TypeRef, TypeRef) = default;
};

/// A library's version, for an Opaque descriptor.
struct Version {
    std::uint64_t major = 0, minor = 0, patch = 0;
    friend bool operator==(Version const&, Version const&) = default;
};

/// A named field of a struct.
struct Field {
    std::string name;
    TypeRef type;
    friend bool operator==(Field const&, Field const&) = default;
};

/// A variant of an enum: its discriminant, its name, and the one value it
/// holds (the empty struct for a variant without fields).
struct Variant {
    std::uint64_t discriminant = 0;
    std::string name;
    TypeRef payload;
    friend bool operator==(Variant const&, Variant const&) = default;
};

/// The primitive types, by their codes in the canonical encoding.
enum class Primitive : std::uint8_t {
    u8 = 0, u16 = 1, u32 = 2, u64 = 3,
    i8 = 4, i16 = 5, i32 = 6, i64 = 7,
    f32 = 8, f64 = 9, boolean = 10, character = 11,
};

/// The size every value of `p` encodes to, if all encode to one size.
std::optional<std::uint64_t> fixed_size(Primitive p);
/// The size of `p`'s fixed form.
std::uint64_t fixed_form_size(Primitive p);

struct Struct {
    std::string name;
    std::vector<Field> fields;
    friend bool operator==(Struct const&, Struct const&) = default;
};
struct Enum {
    std::string name;
    std::uint8_t discriminant_width = 1;
    std::vector<Variant> variants;
    friend bool operator==(Enum const&, Enum const&) = default;
};
struct Opaque {
    std::string library_name;
    std::string type_name;
    Version version;
    std::optional<std::uint64_t> fixed_size;
    std::vector<TypeRef> parameters;
    friend bool operator==(Opaque const&, Opaque const&) = default;
};
struct Array {
    TypeRef element;
    std::uint64_t length = 0;
    friend bool operator==(Array const&, Array const&) = default;
};
struct Sequence {
    TypeRef element;
    friend bool operator==(Sequence const&, Sequence const&) = default;
};
struct Map {
    TypeRef key;
    TypeRef value;
    friend bool operator==(Map const&, Map const&) = default;
};
struct Text {
    friend bool operator==(Text const&, Text const&) = default;
};
struct Fixed {
    TypeRef inner;
    friend bool operator==(Fixed const&, Fixed const&) = default;
};

/// One type descriptor: one of the nine kinds.
using TypeDescriptor =
    std::variant<Primitive, Struct, Enum, Opaque, Array, Sequence, Map, Text, Fixed>;

inline constexpr std::uint8_t tag_struct = 128;
inline constexpr std::uint8_t tag_enum = 129;
inline constexpr std::uint8_t tag_opaque = 130;
inline constexpr std::uint8_t tag_array = 131;
inline constexpr std::uint8_t tag_map = 132;
inline constexpr std::uint8_t tag_sequence = 133;
inline constexpr std::uint8_t tag_text = 134;
inline constexpr std::uint8_t tag_fixed = 135;

/// A type's fingerprint: the first 16 bytes of the SHA-256 of its canonical
/// token stream.
struct Fingerprint {
    std::array<std::uint8_t, 16> bytes{};
    std::string to_hex() const { return kladde::to_hex(bytes); }
    friend bool operator==(Fingerprint const&, Fingerprint const&) = default;
};

/// A root type and every type it reaches: descriptor 0 is the root.
///
/// ```cpp
/// using namespace kladde::schema;
/// TypeTable t({Struct{"Point", {{"x", {1}}, {"y", {1}}}}, Primitive::i32});
/// auto bytes = t.encode();
/// assert(TypeTable::decode(bytes).value() == t);
/// ```
class TypeTable {
public:
    TypeTable() = default;
    explicit TypeTable(std::vector<TypeDescriptor> descriptors)
        : descriptors_(std::move(descriptors)) {}

    std::vector<TypeDescriptor> const& descriptors() const { return descriptors_; }
    TypeDescriptor const& get(TypeRef r) const { return descriptors_.at(r.index); }
    TypeRef root() const { return {0}; }

    /// The canonical encoding of the table (spec/schema/canonical-encoding),
    /// with enum variants in ascending discriminant order whatever order
    /// they are stored in. Byte-identical across conforming implementations.
    ///
    /// ```cpp
    /// TypeTable strings({Sequence{{1}}, Text{}});
    /// assert(strings.encode() == std::vector<std::uint8_t>{2, 133, 1, 134});
    /// ```
    std::vector<std::uint8_t> encode() const;
    /// Decodes a canonical encoding; fails with `errc::corrupt` on anything
    /// else: truncated input, trailing bytes, a varint longer than it needs
    /// to be, a string that is not UTF-8, an unassigned tag or primitive
    /// code, enum variants not in strictly ascending discriminant order, or
    /// a table that `validate` refuses. A table it returns re-encodes to
    /// `bytes`.
    static expected<TypeTable> decode(std::span<std::uint8_t const> bytes);

    /// The fingerprint of the root type (spec/schema/fingerprints): a pure
    /// function of the type graph, independent of descriptor indices, of
    /// sharing, and of the order enum variants are stored in. Requires every
    /// reference to lie in the table, as `validate` checks; throws
    /// `std::out_of_range` otherwise.
    ///
    /// ```cpp
    /// TypeTable map({Map{{1}, {2}}, Text{}, Primitive::u32});
    /// assert(map.fingerprint().to_hex() == "6a33dc4a1e560e465f7a50543f62c492");
    /// ```
    Fingerprint fingerprint() const;
    /// The fingerprint of the type `r` refers to, as if it were the root.
    Fingerprint fingerprint(TypeRef r) const;

    /// The size every value of type `r` encodes to, if all encode to one
    /// (spec/schema/type-descriptors#fixed-size-types). Requires references
    /// in range, as `fingerprint` does.
    ///
    /// ```cpp
    /// TypeTable point({Struct{"Point", {{"x", {1}}, {"y", {1}}}}, Primitive::f32});
    /// assert(point.fixed_size({0}) == 8);
    /// ```
    std::optional<std::uint64_t> fixed_size(TypeRef r) const;
    /// The size of type `r`'s fixed form, the size of a `Fixed` holding it,
    /// if it has one: none for a type that holds a Sequence, a Map, Text, an
    /// Opaque without a fixed size, an enum without variants, or itself, nor
    /// for one whose size exceeds 64 bits. Requires references in range.
    std::optional<std::uint64_t> fixed_form_size(TypeRef r) const;
    /// Checks the rules descriptors must obey beyond their encoding: at
    /// least one descriptor, known primitive codes, references in range,
    /// discriminant widths of 1, 2, 4 or 8 that every discriminant fits,
    /// distinct discriminants, an Opaque size whose size mode fits 64 bits,
    /// a fixed form for whatever a `Fixed` holds, and no type that reaches
    /// itself through Structs, Arrays and `Fixed` alone. Fails with
    /// `errc::corrupt`, naming the first offending descriptor.
    expected<void> validate() const;

    /// The length of the canonical encoding of one value of type `r` at the
    /// front of `bytes`, or `nullopt` if `bytes` does not start with one:
    /// what a reader of a foreign schema skips. Checks what the encoding
    /// constrains: minimal varints in range of their type, discriminants
    /// that name variants, valid UTF-8, `bool`s of 0 or 1, Unicode scalar
    /// values, and zeros as an enum's padding in a fixed form. Refuses a
    /// value nested deeper than `max_measure_depth`. Requires a table that
    /// `validate` accepts.
    ///
    /// ```cpp
    /// TypeTable list({Sequence{{1}}, Primitive::u32});
    /// std::uint8_t two[] = {2, 1, 0xac, 0x02, 99};  // 1 and 300, then more
    /// assert(list.measure({0}, two) == 4);
    /// ```
    std::optional<std::size_t> measure(TypeRef r, std::span<std::uint8_t const> bytes) const;

    /// How deeply `measure` follows a value's nesting before it refuses it.
    static constexpr std::size_t max_measure_depth = 1000;

    friend bool operator==(TypeTable const&, TypeTable const&) = default;

private:
    std::vector<TypeDescriptor> descriptors_;
};

/// SHA-256 of `data` (FIPS 180-4).
///
/// ```cpp
/// std::uint8_t abc[] = {'a', 'b', 'c'};
/// assert(sha256(abc)[0] == 0xba);
/// ```
std::array<std::uint8_t, 32> sha256(std::span<std::uint8_t const> data);

/// An incremental SHA-256: feeding a message in any number of pieces gives
/// the digest of the whole. It buffers one 64-byte block at most.
///
/// ```cpp
/// Sha256 h;
/// h.update(first);
/// h.update(second);
/// auto digest = h.finalize();  // sha256 of first and second together
/// ```
class Sha256 {
public:
    Sha256();
    /// Absorbs `data` into the digest.
    void update(std::span<std::uint8_t const> data);
    /// Absorbs one byte.
    void update(std::uint8_t byte) { update(std::span<std::uint8_t const>(&byte, 1)); }
    /// The digest of everything absorbed. The hasher starts over afterwards,
    /// as if newly constructed.
    std::array<std::uint8_t, 32> finalize();

private:
    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

/// A human-readable dump of a table, one descriptor per line, as the
/// cross-implementation tests compare them. Two tables dump alike exactly
/// when they encode alike: every encoded detail appears, and enum variants
/// appear in ascending discriminant order. Each line is `[i] ` and then
///
/// | kind      | line after `[i] `                                          |
/// | --------- | ---------------------------------------------------------- |
/// | Primitive | `u8`, `u16`, …, `f64`, `bool`, `char`                       |
/// | Struct    | `struct "Name" { "x": #1, "y": #2 }`, or `struct "Unit" {}` |
/// | Enum      | `enum "Name" width 4 { 0 "Nil": #2, 1 "Cons": #3 }`, or `enum "Never" width 1 {}` |
/// | Opaque    | `opaque "lib" "T" 1.2.3 size 8 <#1, #2>`; `size -` without a fixed size, `<>` without parameters |
/// | Array     | `array #1 len 3`                                            |
/// | Sequence  | `sequence #1`                                               |
/// | Map       | `map #1 -> #2`                                              |
/// | Text      | `text`                                                      |
/// | Fixed     | `fixed #1`                                                  |
///
/// and a newline. Strings are quoted, with `"` and `\` escaped as `\"` and
/// `\\`, and bytes below 0x20 and 0x7f as `\xNN`; other bytes, UTF-8 or
/// not, appear as they are. A code that names no primitive, possible only
/// in a table that `validate` refuses, appears as `primitive N`.
///
/// ```cpp
/// TypeTable t({Sequence{{1}}, Text{}});
/// assert(dump(t) == "[0] sequence #1\n[1] text\n");
/// ```
std::string dump(TypeTable const& table);

}  // namespace kladde::schema

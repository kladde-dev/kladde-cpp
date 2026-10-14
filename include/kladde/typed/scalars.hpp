// The primitive types and `enum class`.
//
// | C++ | encoding | fixed form |
// | --- | --- | --- |
// | `std::uint8_t`, `std::int8_t`, `bool` | one byte | the same byte |
// | `std::uint16_t` … `std::uint64_t` | unsigned LEB128 | little-endian |
// | `std::int16_t` … `std::int64_t` | zigzag, then LEB128 | little-endian two's complement |
// | `float`, `double` | IEEE-754, little-endian | the same bytes |
// | `char32_t` | UTF-8 | the scalar value as a little-endian `u32` |
//
// Integers are matched by width and signedness, so `long` is whichever of
// `std::int32_t` and `std::int64_t` it is. Plain `char` is not durable: its
// signedness and its meaning are ambiguous.
#pragma once

#include <algorithm>
#include <array>
#include <limits>

#include <kladde/typed/core.hpp>

namespace kladde {

namespace detail {

template <class T>
constexpr schema::Primitive primitive_of() {
    if constexpr (std::is_same_v<T, bool>) return schema::Primitive::boolean;
    else if constexpr (std::is_same_v<T, char32_t>) return schema::Primitive::character;
    else if constexpr (std::is_same_v<T, float>) return schema::Primitive::f32;
    else if constexpr (std::is_same_v<T, double>) return schema::Primitive::f64;
    else if constexpr (std::is_unsigned_v<T>) {
        if constexpr (sizeof(T) == 1) return schema::Primitive::u8;
        else if constexpr (sizeof(T) == 2) return schema::Primitive::u16;
        else if constexpr (sizeof(T) == 4) return schema::Primitive::u32;
        else return schema::Primitive::u64;
    } else {
        if constexpr (sizeof(T) == 1) return schema::Primitive::i8;
        else if constexpr (sizeof(T) == 2) return schema::Primitive::i16;
        else if constexpr (sizeof(T) == 4) return schema::Primitive::i32;
        else return schema::Primitive::i64;
    }
}

/// The schema's canonical type per primitive, so that `long` and `long
/// long` of one width share a descriptor.
template <class T>
using canonical_scalar = std::conditional_t<
    std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char32_t>,
    std::conditional_t<std::is_signed_v<T>,
                       std::conditional_t<sizeof(T) == 1, std::int8_t,
                       std::conditional_t<sizeof(T) == 2, std::int16_t,
                       std::conditional_t<sizeof(T) == 4, std::int32_t, std::int64_t>>>,
                       std::conditional_t<sizeof(T) == 1, std::uint8_t,
                       std::conditional_t<sizeof(T) == 2, std::uint16_t,
                       std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>>>>,
    T>;

constexpr std::size_t utf8_len(char32_t c) {
    return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
}

inline void put_utf8(Encoder& out, char32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<std::uint8_t>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<std::uint8_t>(0xC0 | (c >> 6)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<std::uint8_t>(0xE0 | (c >> 12)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
    } else {
        out.push_back(static_cast<std::uint8_t>(0xF0 | (c >> 18)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 12) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | ((c >> 6) & 0x3F)));
        out.push_back(static_cast<std::uint8_t>(0x80 | (c & 0x3F)));
    }
}

constexpr bool is_scalar_value(std::uint32_t c) {
    return c < 0x110000 && !(c >= 0xD800 && c < 0xE000);
}

inline bool get_utf8(Decoder& in, char32_t& out) {
    auto b0 = in.byte();
    if (!b0) return false;
    std::size_t n = *b0 < 0x80 ? 1 : (*b0 >> 5) == 6 ? 2 : (*b0 >> 4) == 14 ? 3 : (*b0 >> 3) == 30 ? 4 : 0;
    if (n == 0) return in.fail("a malformed char");
    std::uint32_t c = n == 1 ? *b0 : n == 2 ? (*b0 & 0x1F) : n == 3 ? (*b0 & 0x0F) : (*b0 & 0x07);
    for (std::size_t i = 1; i < n; ++i) {
        auto b = in.byte();
        if (!b) return false;
        if ((*b & 0xC0) != 0x80) return in.fail("a malformed char");
        c = (c << 6) | (*b & 0x3F);
    }
    if (!is_scalar_value(c) || utf8_len(c) != n) return in.fail("a malformed char");
    out = c;
    return true;
}

}  // namespace detail

/// A primitive type the schema names.
template <class T>
concept Scalar = std::is_arithmetic_v<T> && !std::is_same_v<T, char> &&
                 !std::is_same_v<T, wchar_t> && !std::is_same_v<T, char8_t> &&
                 !std::is_same_v<T, char16_t> && !std::is_same_v<T, long double>;

template <Scalar T>
struct durable<T> {
    static constexpr schema::Primitive primitive = detail::primitive_of<T>();
    static constexpr std::optional<std::size_t> fixed_size = []() -> std::optional<std::size_t> {
        if constexpr (sizeof(T) == 1 || std::is_floating_point_v<T>) return sizeof(T);
        else return std::nullopt;
    }();
    static constexpr std::size_t fixed_form_size =
        std::is_same_v<T, char32_t> ? 4 : sizeof(T);

    static std::size_t encoded_len(T const& v) {
        if constexpr (sizeof(T) == 1 || std::is_floating_point_v<T>) return sizeof(T);
        else if constexpr (std::is_same_v<T, char32_t>) return detail::utf8_len(v);
        else if constexpr (std::is_signed_v<T>) return varint_len(zigzag(v));
        else return varint_len(v);
    }
    static void encode(T const& v, Encoder& out) {
        if constexpr (std::is_same_v<T, bool>) out.push_back(v ? 1 : 0);
        else if constexpr (sizeof(T) == 1) out.push_back(static_cast<std::uint8_t>(v));
        else if constexpr (std::is_same_v<T, float>) put_le(out, std::bit_cast<std::uint32_t>(v));
        else if constexpr (std::is_same_v<T, double>) put_le(out, std::bit_cast<std::uint64_t>(v));
        else if constexpr (std::is_same_v<T, char32_t>) detail::put_utf8(out, v);
        else if constexpr (std::is_signed_v<T>) put_varint(out, zigzag(v));
        else put_varint(out, v);
    }
    static bool decode(Decoder& in, T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            auto b = in.byte();
            if (!b) return false;
            if (*b > 1) return in.fail(std::to_string(*b) + " is not a bool");
            v = *b == 1;
            return true;
        } else if constexpr (sizeof(T) == 1) {
            auto b = in.byte();
            if (!b) return false;
            v = static_cast<T>(*b);
            return true;
        } else if constexpr (std::is_same_v<T, float>) {
            auto u = in.le(4);
            if (!u) return false;
            v = std::bit_cast<float>(static_cast<std::uint32_t>(*u));
            return true;
        } else if constexpr (std::is_same_v<T, double>) {
            auto u = in.le(8);
            if (!u) return false;
            v = std::bit_cast<double>(*u);
            return true;
        } else if constexpr (std::is_same_v<T, char32_t>) {
            return detail::get_utf8(in, v);
        } else {
            auto u = in.varint();
            if (!u) return false;
            if constexpr (std::is_signed_v<T>) {
                auto s = unzigzag(*u);
                if (s < std::numeric_limits<T>::min() || s > std::numeric_limits<T>::max())
                    return in.fail("an integer out of range");
                v = static_cast<T>(s);
            } else {
                if (*u > std::numeric_limits<T>::max()) return in.fail("an integer out of range");
                v = static_cast<T>(*u);
            }
            return true;
        }
    }
    static void encode_fixed(T const& v, Encoder& out) {
        if constexpr (std::is_same_v<T, bool>) out.push_back(v ? 1 : 0);
        else if constexpr (std::is_same_v<T, float>) put_le(out, std::bit_cast<std::uint32_t>(v));
        else if constexpr (std::is_same_v<T, double>) put_le(out, std::bit_cast<std::uint64_t>(v));
        else put_le(out, static_cast<std::make_unsigned_t<std::conditional_t<std::is_same_v<T, char32_t>, std::uint32_t, T>>>(v), fixed_form_size);
    }
    static bool decode_fixed(Decoder& in, T& v) {
        if constexpr (std::is_same_v<T, bool> || std::is_floating_point_v<T>) {
            return decode(in, v);
        } else {
            auto u = in.le(fixed_form_size);
            if (!u) return false;
            if constexpr (std::is_same_v<T, char32_t>) {
                if (!detail::is_scalar_value(static_cast<std::uint32_t>(*u)))
                    return in.fail("a malformed char");
            }
            v = static_cast<T>(*u);
            return true;
        }
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<detail::canonical_scalar<T>>,
                          [](SchemaBuilder&) -> schema::TypeDescriptor { return primitive; });
    }
    using mut = SetMut<T>;
};

/// The empty value: the payload of an enum's variants without fields, and
/// the schema's empty struct.
struct Unit {
    friend bool operator==(Unit, Unit) = default;
};

template <>
struct durable<Unit> {
    static constexpr std::optional<std::size_t> fixed_size = 0;
    static constexpr std::size_t fixed_form_size = 0;
    static std::size_t encoded_len(Unit) { return 0; }
    static void encode(Unit, Encoder&) {}
    static bool decode(Decoder&, Unit&) { return true; }
    static void encode_fixed(Unit, Encoder&) {}
    static bool decode_fixed(Decoder&, Unit&) { return true; }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<Unit>, [](SchemaBuilder&) -> schema::TypeDescriptor {
            return schema::Struct{"Tuple0", {}};
        });
    }
    using mut = SetMut<Unit>;
};

// ---------------------------------------------------------------------------
// enum class

/// What `KLADDE_ENUM` describes: an enumeration's name and its enumerators
/// with their names.
template <class E, std::size_t N>
struct enum_description {
    std::string_view name;
    std::array<std::pair<E, std::string_view>, N> enumerators;
};

template <class E, std::size_t N>
constexpr enum_description<E, N> describe_enum(std::string_view name,
                                               std::array<std::pair<E, std::string_view>, N> es) {
    return {name, es};
}

template <class E>
concept DescribedEnum =
    std::is_enum_v<E> && requires { kladde_describe_enum(static_cast<E*>(nullptr)); };

template <DescribedEnum E>
struct durable<E> {
    static constexpr auto description = kladde_describe_enum(static_cast<E*>(nullptr));
    static constexpr std::uint64_t largest = [] {
        std::uint64_t m = 0;
        for (auto& [e, n] : description.enumerators) {
            if (static_cast<long long>(e) < 0) throw "KLADDE_ENUM: negative enumerators are not durable";
            m = std::max(m, static_cast<std::uint64_t>(e));
        }
        return m;
    }();
    static constexpr std::uint8_t width = discriminant_width(largest);
    static constexpr std::optional<std::size_t> fixed_size =
        largest < 128 ? std::optional<std::size_t>(1) : std::nullopt;
    static constexpr std::size_t fixed_form_size = width;

    static std::size_t encoded_len(E const& v) { return varint_len(static_cast<std::uint64_t>(v)); }
    static void encode(E const& v, Encoder& out) { put_varint(out, static_cast<std::uint64_t>(v)); }
    static bool known(std::uint64_t d, E& v) {
        for (auto& [e, n] : description.enumerators) {
            if (static_cast<std::uint64_t>(e) == d) {
                v = e;
                return true;
            }
        }
        return false;
    }
    static bool decode(Decoder& in, E& v) {
        auto d = in.varint();
        if (!d) return false;
        return known(*d, v) || in.fail("an unknown discriminant " + std::to_string(*d));
    }
    static void encode_fixed(E const& v, Encoder& out) {
        put_le(out, static_cast<std::uint64_t>(v), width);
    }
    static bool decode_fixed(Decoder& in, E& v) {
        auto d = in.le(width);
        if (!d) return false;
        return known(*d, v) || in.fail("an unknown discriminant " + std::to_string(*d));
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<E>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            schema::Enum en{std::string(description.name), width, {}};
            auto unit = durable<Unit>::describe(bb);
            for (auto& [e, n] : description.enumerators)
                en.variants.push_back({static_cast<std::uint64_t>(e), std::string(n), unit});
            return en;
        });
    }
    using mut = SetMut<E>;
};

}  // namespace kladde

/// Makes an `enum class` durable: its name, then its enumerators, which must
/// be non-negative. Write it in the enumeration's namespace.
///
/// ```cpp
/// enum class Join : std::uint8_t { miter, round, bevel };
/// KLADDE_ENUM(Join, miter, round, bevel)
/// ```
///
/// The discriminant is the enumerator's value and the variant's name its
/// identifier. The fixed form's discriminant takes the smallest of 1, 2, 4
/// and 8 bytes that holds the largest value, as for a Rust enum without a
/// `#[repr]`.
#define KLADDE_ENUM(Type, ...)                                                          \
    constexpr auto kladde_describe_enum(Type*) {                                        \
        using kladde_enum_type_ = Type;                                                 \
        return ::kladde::describe_enum(                                                 \
            #Type, std::to_array<std::pair<Type, std::string_view>>(                    \
                       {KLADDE_FOR_EACH(KLADDE_ENUMERATOR_, __VA_ARGS__)}));            \
    }
#define KLADDE_ENUMERATOR_(e) std::pair<kladde_enum_type_, std::string_view>{kladde_enum_type_::e, #e},

// A variadic for-each over up to 64 arguments.
#define KLADDE_FOR_EACH(m, ...) __VA_OPT__(KLADDE_FE_EXPAND(KLADDE_FE_HELPER(m, __VA_ARGS__)))
#define KLADDE_FE_HELPER(m, a1, ...) m(a1) __VA_OPT__(KLADDE_FE_AGAIN KLADDE_FE_PARENS(m, __VA_ARGS__))
#define KLADDE_FE_PARENS ()
#define KLADDE_FE_AGAIN() KLADDE_FE_HELPER
#define KLADDE_FE_EXPAND(...) KLADDE_FE_EXPAND4(KLADDE_FE_EXPAND4(KLADDE_FE_EXPAND4(KLADDE_FE_EXPAND4(__VA_ARGS__))))
#define KLADDE_FE_EXPAND4(...) KLADDE_FE_EXPAND3(KLADDE_FE_EXPAND3(KLADDE_FE_EXPAND3(KLADDE_FE_EXPAND3(__VA_ARGS__))))
#define KLADDE_FE_EXPAND3(...) KLADDE_FE_EXPAND2(KLADDE_FE_EXPAND2(KLADDE_FE_EXPAND2(KLADDE_FE_EXPAND2(__VA_ARGS__))))
#define KLADDE_FE_EXPAND2(...) KLADDE_FE_EXPAND1(KLADDE_FE_EXPAND1(KLADDE_FE_EXPAND1(KLADDE_FE_EXPAND1(__VA_ARGS__))))
#define KLADDE_FE_EXPAND1(...) __VA_ARGS__

// Standard types that are durable as they are: `std::optional`, `std::pair`,
// `std::tuple`, `std::array`, `std::vector` of fixed-size elements, and
// `std::variant`; and kladde's `Box` and `Fixed`.
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <tuple>
#include <variant>
#include <vector>

#include <kladde/typed/structs.hpp>
#include <kladde/typed/text.hpp>

namespace kladde {

// ---------------------------------------------------------------------------
// Names

/// The name a type goes by in the schema, where a variant is named after the
/// alternative it holds.
template <class T>
std::string name_of() {
    if constexpr (requires { kladde_name(type_t<T>{}); }) return std::string(kladde_name(type_t<T>{}));
    else if constexpr (DescribedStruct<T>) return std::string(durable<T>::description().name);
    else if constexpr (DescribedEnum<T>) return std::string(durable<T>::description.name);
    else if constexpr (std::is_same_v<T, std::string>) return "Text";
    else if constexpr (Scalar<T>) {
        constexpr char const* names[] = {"u8", "u16", "u32", "u64", "i8", "i16",
                                         "i32", "i64", "f32", "f64", "bool", "char"};
        return names[static_cast<int>(durable<T>::primitive)];
    } else if constexpr (requires { durable<T>::name; }) return std::string(durable<T>::name);
    else return "Value";
}

// ---------------------------------------------------------------------------
// std::optional

/// The handle of a `std::optional`: `set`, `reset`, `emplace`, and the
/// handle of the value it holds.
template <class T>
class OptionalMut : public MutBase<std::optional<T>> {
public:
    using MutBase<std::optional<T>>::MutBase;
    void reset() { this->set(std::nullopt); }
    void emplace(T v) { this->set(std::optional<T>(std::move(v))); }
    bool has_value() const { return this->value_->has_value(); }
    /// The handle of the value it holds, which must be there.
    Mut<T> value() {
        assert(this->value_->has_value());
        auto& v = **this->value_;
        return Mut<T>(v, this->region_.child(1, durable<T>::encoded_len(v), nullptr, 0));
    }
};

/// `std::optional<T>`: an enum `Option`, `None` = 0 holding nothing and
/// `Some` = 1 holding the `T`, as Rust's `Option<T>`.
template <Durable T>
struct durable<std::optional<T>> {
    using O = std::optional<T>;
    static constexpr std::optional<std::size_t> fixed_size =
        durable<T>::fixed_size == std::optional<std::size_t>(0) ? std::optional<std::size_t>(1)
                                                                 : std::nullopt;
    static constexpr bool contains_list = contains_list_v<T>;
    static std::size_t encoded_len(O const& v) { return v ? 1 + durable<T>::encoded_len(*v) : 1; }
    static void encode(O const& v, Encoder& out) {
        out.push_back(v ? 1 : 0);
        if (v) durable<T>::encode(*v, out);
    }
    static bool decode(Decoder& in, O& v) {
        auto d = in.varint();
        if (!d) return false;
        if (*d == 0) {
            v.reset();
            return true;
        }
        if (*d != 1) return in.fail("an unknown discriminant " + std::to_string(*d));
        if constexpr (std::is_default_constructible_v<T>) v.emplace();
        return durable<T>::decode(in, *v);
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<O>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            auto none = durable<Unit>::describe(bb);
            auto some = durable<T>::describe(bb);
            return schema::Enum{"Option", 1, {{0, "None", none}, {1, "Some", some}}};
        });
    }
    static bool locate(O const& v, void const* target, std::uint64_t& offset) {
        if (!v) return false;
        std::uint64_t inner = offset + 1;
        if (static_cast<void const*>(&*v) == target || kladde::locate(*v, target, inner)) {
            offset = inner;
            return true;
        }
        return false;
    }
    static void attach(O& v, Anchor* anchor) {
        if (v) kladde::attach(*v, anchor);
    }
    using mut = OptionalMut<T>;
};

// ---------------------------------------------------------------------------
// std::pair and std::tuple

/// The handle of a tuple: `get<I>()` for one element, `parts()` for all.
template <class Tup>
class TupleMut : public MutBase<Tup> {
public:
    using MutBase<Tup>::MutBase;
    static constexpr std::size_t count = std::tuple_size_v<Tup>;

    template <std::size_t I>
    Mut<std::tuple_element_t<I, Tup>> get() {
        std::uint64_t offset = 0;
        [&]<std::size_t... K>(std::index_sequence<K...>) {
            ((offset += durable<std::tuple_element_t<K, Tup>>::encoded_len(std::get<K>(*this->value_))), ...);
        }(std::make_index_sequence<I>{});
        auto& v = std::get<I>(*this->value_);
        return Mut<std::tuple_element_t<I, Tup>>(
            v, this->region_.child(offset, durable<std::tuple_element_t<I, Tup>>::encoded_len(v), nullptr, I));
    }
    auto parts() {
        std::array<std::optional<std::size_t>, count> fixed{};
        std::array<std::uint64_t, count> sizes{};
        [&]<std::size_t... K>(std::index_sequence<K...>) {
            ((fixed[K] = durable<std::tuple_element_t<K, Tup>>::fixed_size,
              sizes[K] = durable<std::tuple_element_t<K, Tup>>::encoded_len(std::get<K>(*this->value_))), ...);
        }(std::make_index_sequence<count>{});
        fields_.fill(0, fixed, sizes);
        return [&]<std::size_t... K>(std::index_sequence<K...>) {
            return std::tuple<Mut<std::tuple_element_t<K, Tup>>...>(Mut<std::tuple_element_t<K, Tup>>(
                std::get<K>(*this->value_), this->region_.field(fields_, K))...);
        }(std::make_index_sequence<count>{});
    }

private:
    FieldSizes fields_;
};

namespace detail {
template <class Tup>
struct tuple_durable {
    static constexpr std::size_t count = std::tuple_size_v<Tup>;
    template <class F>
    static constexpr void each(F&& f) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (f(std::integral_constant<std::size_t, I>{}), ...);
        }(std::make_index_sequence<count>{});
    }
    template <std::size_t I>
    using elem = std::tuple_element_t<I, Tup>;

    static constexpr std::optional<std::size_t> fixed_size = []<std::size_t... I>(std::index_sequence<I...>) {
        return sum_sizes({durable<std::tuple_element_t<I, Tup>>::fixed_size...});
    }(std::make_index_sequence<count>{});
    static constexpr bool contains_list = []<std::size_t... I>(std::index_sequence<I...>) {
        return (contains_list_v<std::tuple_element_t<I, Tup>> || ... || false);
    }(std::make_index_sequence<count>{});
    static std::size_t encoded_len(Tup const& v) {
        std::size_t n = 0;
        each([&](auto i) { n += durable<elem<i>>::encoded_len(std::get<i>(v)); });
        return n;
    }
    static void encode(Tup const& v, Encoder& out) {
        each([&](auto i) { durable<elem<i>>::encode(std::get<i>(v), out); });
    }
    static bool decode(Decoder& in, Tup& v) {
        bool ok = true;
        each([&](auto i) { ok = ok && durable<elem<i>>::decode(in, std::get<i>(v)); });
        return ok;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<Tup>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            schema::Struct s{"Tuple" + std::to_string(count), {}};
            each([&](auto i) { s.fields.push_back({std::to_string(i), durable<elem<i>>::describe(bb)}); });
            return s;
        });
    }
    static bool locate(Tup const& v, void const* target, std::uint64_t& offset) {
        bool found = false;
        each([&](auto i) {
            if (found) return;
            auto const& f = std::get<i>(v);
            std::uint64_t inner = offset;
            if (static_cast<void const*>(&f) == target || kladde::locate(f, target, inner)) {
                offset = inner;
                found = true;
                return;
            }
            offset += durable<elem<i>>::encoded_len(f);
        });
        return found;
    }
    static void attach(Tup& v, Anchor* anchor) {
        each([&](auto i) { kladde::attach(std::get<i>(v), anchor); });
    }
    using mut = TupleMut<Tup>;
};
}  // namespace detail

/// `std::pair` and `std::tuple`: a struct `TupleN` of positional fields
/// `"0"`, `"1"`, …, as Rust's tuples.
template <Durable... Ts>
struct durable<std::tuple<Ts...>> : detail::tuple_durable<std::tuple<Ts...>> {};
template <Durable A, Durable B>
struct durable<std::pair<A, B>> : detail::tuple_durable<std::pair<A, B>> {};

// ---------------------------------------------------------------------------
// std::array

/// The handle of a `std::array`: `at(i)` for one element, and `set`.
template <class T, std::size_t N>
class ArrayMut : public MutBase<std::array<T, N>> {
public:
    using MutBase<std::array<T, N>>::MutBase;
    Mut<T> at(std::size_t i) {
        assert(i < N);
        std::uint64_t offset = 0;
        if constexpr (durable<T>::fixed_size) {
            offset = i * *durable<T>::fixed_size;
        } else {
            for (std::size_t k = 0; k < i; ++k) offset += durable<T>::encoded_len((*this->value_)[k]);
        }
        auto& v = (*this->value_)[i];
        return Mut<T>(v, this->region_.child(offset, durable<T>::encoded_len(v), nullptr, i));
    }
};

/// `std::array<T, N>`: an Array, the `N` elements back to back.
template <Durable T, std::size_t N>
struct durable<std::array<T, N>> {
    using A = std::array<T, N>;
    static constexpr std::optional<std::size_t> fixed_size =
        durable<T>::fixed_size ? std::optional<std::size_t>(*durable<T>::fixed_size * N) : std::nullopt;
    static constexpr bool contains_list = contains_list_v<T>;
    static std::size_t encoded_len(A const& v) {
        if constexpr (fixed_size) return *fixed_size;
        std::size_t n = 0;
        for (auto& x : v) n += durable<T>::encoded_len(x);
        return n;
    }
    static void encode(A const& v, Encoder& out) {
        for (auto& x : v) durable<T>::encode(x, out);
    }
    static bool decode(Decoder& in, A& v) {
        for (auto& x : v)
            if (!durable<T>::decode(in, x)) return false;
        return true;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<A>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            return schema::Array{durable<T>::describe(bb), N};
        });
    }
    static bool locate(A const& v, void const* target, std::uint64_t& offset) {
        for (auto& x : v) {
            std::uint64_t inner = offset;
            if (static_cast<void const*>(&x) == target || kladde::locate(x, target, inner)) {
                offset = inner;
                return true;
            }
            offset += durable<T>::encoded_len(x);
        }
        return false;
    }
    static void attach(A& v, Anchor* anchor) {
        for (auto& x : v) kladde::attach(x, anchor);
    }
    using mut = ArrayMut<T, N>;
};

// ---------------------------------------------------------------------------
// std::vector of fixed-size elements

/// The handle of a `std::vector` of fixed-size elements: element `i` lies
/// at `i` times the element size, so every operation finds it at once.
/// Each insertion and removal is one transaction of two records, the
/// element and the count.
///
/// ```cpp
/// struct Path { std::vector<std::pair<float, float>> points; };
/// KLADDE_STRUCT(Path, points)
/// auto p = file.edit();
/// p.points().push_back({1, 2});
/// p.points().at(0).set({3, 4});
/// ```
template <class T>
class StdVectorMut : public MutBase<std::vector<T>> {
    static constexpr std::size_t size = *durable<T>::fixed_size;

public:
    using MutBase<std::vector<T>>::MutBase;

    Mut<T> at(std::size_t i) {
        assert(i < this->value_->size());
        auto& v = (*this->value_)[i];
        return Mut<T>(v, this->region_.child(offset_of(i), size, nullptr, i));
    }
    void insert(std::size_t i, T value) {
        auto n = this->value_->size();
        if (i > n) return report(error{errc::out_of_bounds});
        this->value_->reserve(n + 1);
        auto bytes = to_bytes(value);
        auto at = offset_of(i);
        auto r = this->region_.transaction([&]() -> expected<void> {
            KLADDE_TRY(this->region_.splice(at, 0, bytes));
            return write_count(n, n + 1);
        });
        if (!check(this->region_.backend(), r)) return;
        this->value_->insert(this->value_->begin() + static_cast<std::ptrdiff_t>(i), std::move(value));
    }
    void push_back(T value) { insert(this->value_->size(), std::move(value)); }
    void erase(std::size_t i) {
        auto n = this->value_->size();
        if (i >= n) return report(error{errc::out_of_bounds});
        auto at = offset_of(i);
        auto r = this->region_.transaction([&]() -> expected<void> {
            KLADDE_TRY(this->region_.splice(at, size, {}));
            return write_count(n, n - 1);
        });
        if (!check(this->region_.backend(), r)) return;
        this->value_->erase(this->value_->begin() + static_cast<std::ptrdiff_t>(i));
    }
    void pop_back() {
        if (!this->value_->empty()) erase(this->value_->size() - 1);
    }
    void clear() { this->set({}); }

private:
    std::uint64_t offset_of(std::size_t i) const { return varint_len(this->value_->size()) + i * size; }
    expected<void> write_count(std::size_t old_n, std::size_t new_n) {
        Encoder b;
        put_varint(b, new_n);
        auto r = this->region_.splice(0, varint_len(old_n), b);
        if (!r) return std::move(r).error();
        return {};
    }
};

/// `std::vector<T>` for a fixed-size `T`: a Sequence, the count and then the
/// elements. A vector of elements whose size depends on their value is
/// refused at compile time: use `kladde::Vector<T>`, which keeps an index of
/// where its elements start.
template <Durable T>
    requires(!std::is_same_v<T, bool>)
struct durable<std::vector<T>> {
    static_assert(durable<T>::fixed_size.has_value(),
                  "a std::vector<T> is durable only for a fixed-size T; "
                  "use kladde::Vector<T> for elements whose size depends on their value");
    static_assert(!contains_list_v<T>, "a std::vector cannot hold a kladde::List");
    using V = std::vector<T>;
    static constexpr std::size_t size = *durable<T>::fixed_size;
    static constexpr std::optional<std::size_t> fixed_size = std::nullopt;
    static std::size_t encoded_len(V const& v) { return varint_len(v.size()) + v.size() * size; }
    static void encode(V const& v, Encoder& out) {
        put_varint(out, v.size());
        for (auto& x : v) durable<T>::encode(x, out);
    }
    static bool decode(Decoder& in, V& v) {
        auto n = in.varint();
        if (!n) return false;
        v.clear();
        v.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(*n, 1 << 16)));
        for (std::uint64_t i = 0; i < *n; ++i) {
            T x{};
            if (!durable<T>::decode(in, x)) return false;
            v.push_back(std::move(x));
        }
        return true;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<V>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            return schema::Sequence{durable<T>::describe(bb)};
        });
    }
    using mut = StdVectorMut<T>;
};

// ---------------------------------------------------------------------------
// kladde::Box

/// A value on the heap, never null: how a recursive type gets a size, as
/// Rust's `Box`. Durable as what it holds, so boxing a value changes nothing
/// on file.
template <class T>
class Box {
public:
    Box() : p_(std::make_unique<T>()) {}
    Box(T value) : p_(std::make_unique<T>(std::move(value))) {}
    Box(Box const& o) : p_(std::make_unique<T>(*o.p_)) {}
    Box(Box&&) noexcept = default;
    Box& operator=(Box const& o) {
        *p_ = *o.p_;
        return *this;
    }
    Box& operator=(Box&& o) noexcept = default;
    T& operator*() { return *p_; }
    T const& operator*() const { return *p_; }
    T* operator->() { return p_.get(); }
    T const* operator->() const { return p_.get(); }
    T* get() { return p_.get(); }
    T const* get() const { return p_.get(); }
    friend bool operator==(Box const& a, Box const& b) { return *a == *b; }

private:
    std::unique_ptr<T> p_;
};

template <class T>
struct durable<Box<T>> {
    // Not `T`'s fixed size: a recursive type reaches itself through a box,
    // and asking would make the compiler instantiate it inside itself.
    static constexpr std::optional<std::size_t> fixed_size = std::nullopt;
    static constexpr bool contains_list = contains_list_v<T>;
    static std::size_t encoded_len(Box<T> const& v) { return durable<T>::encoded_len(*v); }
    static void encode(Box<T> const& v, Encoder& out) { durable<T>::encode(*v, out); }
    static bool decode(Decoder& in, Box<T>& v) { return durable<T>::decode(in, *v); }
    static schema::TypeRef describe(SchemaBuilder& b) { return durable<T>::describe(b); }
    static bool locate(Box<T> const& v, void const* target, std::uint64_t& offset) {
        if (static_cast<void const*>(v.get()) == target) return true;
        return kladde::locate(*v, target, offset);
    }
    static void attach(Box<T>& v, Anchor* anchor) { kladde::attach(*v, anchor); }
    class mut : public Mut<T> {
    public:
        mut(Box<T>& value, Region region) : Mut<T>(*value, std::move(region)) {}
    };
};

// ---------------------------------------------------------------------------
// kladde::Fixed

/// A value held in its fixed form: an encoding of one size for every value,
/// so that it can be overwritten in place and lies at a constant offset.
template <Bounded T>
struct Fixed {
    T value{};
    friend bool operator==(Fixed const&, Fixed const&) = default;
};

template <Bounded T>
struct durable<Fixed<T>> {
    static constexpr std::optional<std::size_t> fixed_size = durable<T>::fixed_form_size;
    static std::size_t encoded_len(Fixed<T> const&) { return durable<T>::fixed_form_size; }
    static void encode(Fixed<T> const& v, Encoder& out) { durable<T>::encode_fixed(v.value, out); }
    static bool decode(Decoder& in, Fixed<T>& v) { return durable<T>::decode_fixed(in, v.value); }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<Fixed<T>>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            return schema::Fixed{durable<T>::describe(bb)};
        });
    }
    class mut : public SetMut<Fixed<T>> {
    public:
        using SetMut<Fixed<T>>::SetMut;
        /// Overwrites the value in place: one Write of its fixed form.
        void set(Fixed<T> v) {
            auto bytes = to_bytes(v);
            if (!check(this->region_.backend(), this->region_.write(0, bytes))) return;
            *this->value_ = std::move(v);
        }
    };
};

// ---------------------------------------------------------------------------
// std::variant

/// The variants of a `std::variant`: their discriminants, which default to
/// the alternatives' indices, and the enum's name. `KLADDE_VARIANT` pins
/// them.
template <class V>
struct variant_info {
    static constexpr std::string_view name = "Variant";
    static constexpr std::uint64_t discriminant(std::size_t index) { return index; }
    /// The variant's name, or empty for the alternative's own name.
    static constexpr std::string_view variant_name(std::size_t) { return {}; }
};

/// The schema's name of alternative `I` of `V`: the one `KLADDE_VARIANT`
/// gives it, or else its type's.
template <class V, std::size_t I>
std::string variant_name_of() {
    auto n = variant_info<V>::variant_name(I);
    if (!n.empty()) return std::string(n);
    return name_of<std::variant_alternative_t<I, V>>();
}

/// The handle of a `std::variant`: `set` to switch alternatives, `visit` to
/// reach the current alternative's handle, `if_holds<A>(f)` to reach it if
/// it is an `A`.
///
/// ```cpp
/// using PhoneNumber = std::variant<Mobile, Landline>;
/// number.visit([](auto alt) { alt.digits().append("0"); });
/// number.if_holds<Mobile>([](kladde::Mut<Mobile> m) { m.digits().set("555"); });
/// ```
template <class... Ts>
class VariantMut : public MutBase<std::variant<Ts...>> {
    using V = std::variant<Ts...>;

public:
    using MutBase<V>::MutBase;

    std::size_t index() const { return this->value_->index(); }

    template <class F>
    decltype(auto) visit(F&& f) {
        auto disc = variant_info<V>::discriminant(this->value_->index());
        auto at = varint_len(disc);
        return std::visit(
            [&](auto& alt) -> decltype(auto) {
                using A = std::remove_cvref_t<decltype(alt)>;
                return f(Mut<A>(alt, this->region_.child(at, durable<A>::encoded_len(alt), nullptr, 0)));
            },
            *this->value_);
    }

    template <class A, class F>
    bool if_holds(F&& f) {
        auto* alt = std::get_if<A>(this->value_);
        if (!alt) return false;
        auto at = varint_len(variant_info<V>::discriminant(this->value_->index()));
        f(Mut<A>(*alt, this->region_.child(at, durable<A>::encoded_len(*alt), nullptr, 0)));
        return true;
    }
};

template <Durable... Ts>
struct durable<std::variant<Ts...>> {
    using V = std::variant<Ts...>;
    static constexpr std::size_t count = sizeof...(Ts);
    template <std::size_t I>
    using alt = std::variant_alternative_t<I, V>;

    static constexpr std::optional<std::size_t> fixed_size = []() -> std::optional<std::size_t> {
        std::optional<std::size_t> sizes[] = {durable<Ts>::fixed_size...};
        for (std::size_t i = 0; i < count; ++i) {
            if (!sizes[i] || sizes[i] != sizes[0]) return std::nullopt;
            if (variant_info<V>::discriminant(i) >= 128) return std::nullopt;
        }
        return 1 + *sizes[0];
    }();
    static constexpr bool contains_list = (contains_list_v<Ts> || ...);

    static std::uint64_t disc(V const& v) { return variant_info<V>::discriminant(v.index()); }
    static std::size_t encoded_len(V const& v) {
        return varint_len(disc(v)) +
               std::visit([](auto const& a) { return durable<std::remove_cvref_t<decltype(a)>>::encoded_len(a); }, v);
    }
    static void encode(V const& v, Encoder& out) {
        put_varint(out, disc(v));
        std::visit([&](auto const& a) { durable<std::remove_cvref_t<decltype(a)>>::encode(a, out); }, v);
    }
    static bool decode(Decoder& in, V& v) {
        auto d = in.varint();
        if (!d) return false;
        bool found = false, ok = false;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((!found && variant_info<V>::discriminant(I) == *d
                  ? (found = true, ok = durable<alt<I>>::decode(in, v.template emplace<I>()), 0)
                  : 0),
             ...);
        }(std::make_index_sequence<count>{});
        if (!found) return in.fail("an unknown discriminant " + std::to_string(*d));
        return ok;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<V>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            schema::Enum e{std::string(variant_info<V>::name), 1, {}};
            std::uint64_t largest = 0;
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((e.variants.push_back({variant_info<V>::discriminant(I), variant_name_of<V, I>(),
                                        durable<alt<I>>::describe(bb)}),
                  largest = std::max(largest, variant_info<V>::discriminant(I))),
                 ...);
            }(std::make_index_sequence<count>{});
            e.discriminant_width = discriminant_width(largest);
            return e;
        });
    }
    static bool locate(V const& v, void const* target, std::uint64_t& offset) {
        std::uint64_t inner = offset + varint_len(disc(v));
        bool found = std::visit(
            [&](auto const& a) {
                return static_cast<void const*>(&a) == target || kladde::locate(a, target, inner);
            },
            v);
        if (found) offset = inner;
        return found;
    }
    static void attach(V& v, Anchor* anchor) {
        std::visit([&](auto& a) { kladde::attach(a, anchor); }, v);
    }
    using mut = VariantMut<Ts...>;
};

}  // namespace kladde

/// Names a `std::variant` in the schema and pins its discriminants, so that
/// inserting an alternative does not renumber the ones after it. Write it at
/// global scope.
///
/// ```cpp
/// using PhoneNumber = std::variant<Mobile, Landline>;
/// KLADDE_VARIANT(PhoneNumber, "PhoneNumber", 1, 2)
/// ```
#define KLADDE_VARIANT(Type, Name, ...)                                           \
    template <>                                                                   \
    struct kladde::variant_info<Type> {                                           \
        static constexpr std::string_view name = Name;                            \
        static constexpr std::uint64_t discriminants[] = {__VA_ARGS__};           \
        static_assert(std::size(discriminants) == std::variant_size_v<Type>,      \
                      "one discriminant per alternative");                        \
        static constexpr std::uint64_t discriminant(std::size_t index) {          \
            return discriminants[index];                                          \
        }                                                                         \
        static constexpr std::string_view variant_name(std::size_t) { return {}; } \
    };

/// Names a `std::variant` and each of its variants in the schema, and pins
/// their discriminants. Variant names are part of a type's fingerprint, so
/// a file that another program writes from an enum whose variants are named
/// otherwise than the alternatives' types — Rust's `Poly(Vec<Point>)` held
/// as a `std::vector<Point>` — opens only with the names it uses. Write it
/// at global scope.
///
/// ```cpp
/// using Shape = std::variant<Circle, std::vector<Point>, std::string, kladde::Unit>;
/// KLADDE_VARIANT_NAMED(Shape, "Shape", (0, "Circle"), (1, "Poly"), (2, "Label"), (3, "Empty"))
/// ```
#define KLADDE_VARIANT_NAMED(Type, Name, ...)                                        \
    template <>                                                                      \
    struct kladde::variant_info<Type> {                                              \
        static constexpr std::string_view name = Name;                               \
        static constexpr std::pair<std::uint64_t, std::string_view> variants[] = {   \
            KLADDE_FOR_EACH(KLADDE_VARIANT_PAIR_, __VA_ARGS__)};                     \
        static_assert(std::size(variants) == std::variant_size_v<Type>,              \
                      "one discriminant and name per alternative");                  \
        static constexpr std::uint64_t discriminant(std::size_t index) {             \
            return variants[index].first;                                            \
        }                                                                            \
        static constexpr std::string_view variant_name(std::size_t index) {          \
            return variants[index].second;                                           \
        }                                                                            \
    };
#define KLADDE_VARIANT_PAIR_(p) std::pair<std::uint64_t, std::string_view> p,

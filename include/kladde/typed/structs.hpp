// Structs: described by `KLADDE_STRUCT` (or `KLADDE_MEMBERS` inside the
// class), encoded as their fields back to back, and mutated through a
// `Mut` with an accessor per field.
#pragma once

#include <tuple>

#include <kladde/typed/scalars.hpp>

namespace kladde {

/// A tag for a type, by which argument-dependent lookup finds the
/// descriptions that `KLADDE_STRUCT` writes in the type's namespace.
template <class T>
struct type_t {};

namespace detail {
template <class C, class F>
F member_type_of(F C::*);
template <class C, class F>
C class_of(F C::*);
}  // namespace detail

/// The type of the member that pointer `M` names.
template <auto M>
using member_t = decltype(detail::member_type_of(M));

/// One persisted member: its pointer and its name on file.
template <auto M>
struct field_ref {
    std::string_view name;
};

template <auto M>
constexpr field_ref<M> field(std::string_view name) {
    return {name};
}

/// A struct's description: its name, and its persisted members in encoding
/// order.
template <class... Fs>
struct struct_description {
    std::string_view name;
    std::tuple<Fs...> fields;
};

template <class... Fs>
constexpr struct_description<Fs...> describe_struct(std::string_view name, Fs... fs) {
    return {name, std::tuple<Fs...>(fs...)};
}

template <class T>
concept DescribedStruct = requires { kladde_describe(type_t<T>{}); };

template <class T>
concept TransparentStruct = requires { kladde_transparent(type_t<T>{}); };

namespace detail {

template <class F>
struct field_traits;
template <auto M>
struct field_traits<field_ref<M>> {
    static constexpr auto pointer = M;
    using type = member_t<M>;
};

/// What follows from a struct's fields at compile time, in a class of its
/// own: a static member of `durable<T>` cannot be computed by calling
/// `durable<T>`'s own functions, which are not defined until it is complete.
template <class Fields>
struct struct_info;
template <class... Fs>
struct struct_info<std::tuple<Fs...>> {
    static constexpr std::optional<std::size_t> fixed_size =
        sum_sizes({durable<typename field_traits<Fs>::type>::fixed_size...});
    static constexpr bool contains_list = (contains_list_v<typename field_traits<Fs>::type> || ... || false);
    template <auto M>
    static constexpr std::size_t index_of() {
        constexpr bool same[] = {std::is_same_v<Fs, field_ref<M>>..., false};
        for (std::size_t i = 0; i < sizeof...(Fs); ++i)
            if (same[i]) return i;
        throw "the member is not among the struct's described fields";
    }
};

struct no_accessors {};

template <class T, class Self>
auto accessors_of(int) -> decltype(kladde_accessors(type_t<T>{}, static_cast<Self*>(nullptr)));
template <class T, class Self>
no_accessors accessors_of(...);

}  // namespace detail

/// The accessors `KLADDE_STRUCT` generates for `T`'s handle, which give it a
/// method per field.
template <class T, class Self>
using accessors_t = decltype(detail::accessors_of<T, Self>(0));

template <DescribedStruct T>
struct durable<T> {
    // A function rather than a `static constexpr auto` member, which makes
    // GCC 13 fail with an internal error.
    static constexpr auto description() { return kladde_describe(type_t<T>{}); }
    using fields_tuple = std::remove_cvref_t<decltype(kladde_describe(type_t<T>{}).fields)>;
    static constexpr std::size_t count = std::tuple_size_v<fields_tuple>;

    template <std::size_t I>
    using field_t = typename detail::field_traits<std::tuple_element_t<I, fields_tuple>>::type;
    template <std::size_t I>
    static constexpr auto pointer = detail::field_traits<std::tuple_element_t<I, fields_tuple>>::pointer;

    template <class F>
    static constexpr void each(F&& f) {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (f(std::integral_constant<std::size_t, I>{}), ...);
        }(std::make_index_sequence<count>{});
    }

    using info = detail::struct_info<fields_tuple>;

    /// The index of the field `M` names.
    template <auto M>
    static constexpr std::size_t index_of = info::template index_of<M>();

    static constexpr std::optional<std::size_t> fixed_size = info::fixed_size;
    static constexpr bool contains_list = info::contains_list;

    static std::size_t encoded_len(T const& v) {
        std::size_t n = 0;
        each([&](auto i) { n += durable<field_t<i>>::encoded_len(v.*pointer<i>); });
        return n;
    }
    static void encode(T const& v, Encoder& out) {
        each([&](auto i) { durable<field_t<i>>::encode(v.*pointer<i>, out); });
    }
    static bool decode(Decoder& in, T& v) {
        bool ok = true;
        each([&](auto i) { ok = ok && durable<field_t<i>>::decode(in, v.*pointer<i>); });
        return ok;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<T>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            constexpr auto d = description();
            schema::Struct s{std::string(d.name), {}};
            each([&](auto i) {
                s.fields.push_back({std::string(std::get<i>(d.fields).name),
                                    durable<field_t<i>>::describe(bb)});
            });
            return s;
        });
    }
    static bool locate(T const& v, void const* target, std::uint64_t& offset) {
        bool found = false;
        each([&](auto i) {
            if (found) return;
            auto const& f = v.*pointer<i>;
            if constexpr (contains_list_v<field_t<i>>) {
                if (static_cast<void const*>(&f) == target) {
                    found = true;
                    return;
                }
                std::uint64_t inner = offset;
                if (kladde::locate(f, target, inner)) {
                    offset = inner;
                    found = true;
                    return;
                }
            }
            offset += durable<field_t<i>>::encoded_len(f);
        });
        return found;
    }
    static void attach(T& v, Anchor* anchor) {
        each([&](auto i) { kladde::attach(v.*pointer<i>, anchor); });
    }

    class mut;
};

/// The handle of a described struct: an accessor per field (`c.email()`),
/// `at<&T::member>()` for any field, `parts()` for every field at once, and
/// a whole-value `set`.
///
/// A field's handle made by its accessor finds where the field starts when
/// it is made; handles made together by `parts()` find it whenever they
/// write, so that writing one field and then another works whatever the
/// first did to its size.
///
/// ```cpp
/// struct Contact { std::string email; bool starred = false; };
/// KLADDE_STRUCT(Contact, email, starred)
///
/// auto c = file.edit();
/// c.starred().set(true);
/// auto [email, starred] = c.parts();
/// email.append(".uk");
/// starred.set(false);
/// ```
template <DescribedStruct T>
class durable<T>::mut : public MutBase<T>, public accessors_t<T, typename durable<T>::mut> {
    using D = durable<T>;

public:
    mut(T& value, Region region) : MutBase<T>(value, std::move(region)) {}

    /// The handle of the field `M` names.
    template <auto M>
    Mut<member_t<M>> at() {
        constexpr std::size_t i = D::template index_of<M>;
        std::uint64_t offset = 0;
        D::each([&](auto k) {
            if constexpr (k < i) offset += durable<typename D::template field_t<k>>::encoded_len(this->value_->*D::template pointer<k>);
        });
        auto& f = this->value_->*M;
        return Mut<member_t<M>>(f, this->region_.child(offset, durable<member_t<M>>::encoded_len(f), nullptr, i));
    }

    /// The handles of every field at once, for a structured binding.
    auto parts() {
        std::array<std::optional<std::size_t>, D::count> fixed{};
        std::array<std::uint64_t, D::count> sizes{};
        D::each([&](auto k) {
            using F = typename D::template field_t<k>;
            fixed[k] = durable<F>::fixed_size;
            sizes[k] = durable<F>::encoded_len(this->value_->*D::template pointer<k>);
        });
        fields_.fill(0, fixed, sizes);
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return std::tuple<Mut<typename D::template field_t<I>>...>(
                Mut<typename D::template field_t<I>>(this->value_->*D::template pointer<I>,
                                                     this->region_.field(fields_, I))...);
        }(std::make_index_sequence<D::count>{});
    }

private:
    FieldSizes fields_;
};

// ---------------------------------------------------------------------------
// Transparent structs

/// A struct of one member, durable exactly as that member: same bytes, same
/// fingerprint. Its handle is the member's.
template <TransparentStruct T>
struct durable<T> {
    static constexpr auto pointer = kladde_transparent(type_t<T>{});
    using F = member_t<pointer>;
    static constexpr std::optional<std::size_t> fixed_size = durable<F>::fixed_size;
    static constexpr bool contains_list = contains_list_v<F>;
    static std::size_t encoded_len(T const& v) { return durable<F>::encoded_len(v.*pointer); }
    static void encode(T const& v, Encoder& out) { durable<F>::encode(v.*pointer, out); }
    static bool decode(Decoder& in, T& v) { return durable<F>::decode(in, v.*pointer); }
    static schema::TypeRef describe(SchemaBuilder& b) { return durable<F>::describe(b); }
    static bool locate(T const& v, void const* target, std::uint64_t& offset) {
        if (static_cast<void const*>(&(v.*pointer)) == target) return true;
        return kladde::locate(v.*pointer, target, offset);
    }
    static void attach(T& v, Anchor* anchor) { kladde::attach(v.*pointer, anchor); }

    class mut : public Mut<F> {
    public:
        mut(T& value, Region region) : Mut<F>(value.*pointer, std::move(region)) {}
    };
};

}  // namespace kladde

/// Makes a struct durable: its name, then the members to persist, in
/// encoding order. Write it next to the struct, in its namespace.
///
/// ```cpp
/// namespace app {
/// struct Contact {
///     std::string email;
///     kladde::Vector<PhoneNumber> phones;
///     bool starred = false;
///     Cache cache;  // not listed, so not persisted
/// };
/// KLADDE_STRUCT(Contact, email, phones, starred)
/// }
/// ```
///
/// It defines what argument-dependent lookup finds: the description, from
/// which encoding, decoding, the descriptor and the fixed size follow, and
/// the accessors that give `kladde::Mut<Contact>` the methods `email()`,
/// `phones()` and `starred()`. A field named like a method of `Mut` itself
/// (`set`, `get`, `at`, `parts`, `region`) is reached by `at<&T::field>()`.
#define KLADDE_STRUCT(Type, ...)                                                      \
    constexpr auto kladde_describe(::kladde::type_t<Type>) {                          \
        using kladde_self_ = Type;                                                    \
        return ::kladde::describe_struct(#Type KLADDE_FOR_EACH(KLADDE_FIELD_, __VA_ARGS__)); \
    }                                                                                 \
    template <class Self>                                                             \
    struct kladde_accessors_##Type {                                                  \
        using kladde_self_ = Type;                                                    \
        KLADDE_FOR_EACH(KLADDE_ACCESSOR_, __VA_ARGS__)                                \
    };                                                                                \
    template <class Self>                                                             \
    kladde_accessors_##Type<Self> kladde_accessors(::kladde::type_t<Type>, Self*);

#define KLADDE_FIELD_(m) , ::kladde::field<&kladde_self_::m>(#m)
#define KLADDE_ACCESSOR_(m)                                                          \
    auto m() { return static_cast<Self&>(*this).template at<&kladde_self_::m>(); }

/// Makes a struct of one member durable exactly as that member, as
/// `#[kladde(transparent)]` does in kladde-rs.
///
/// ```cpp
/// struct Text { std::string content; };
/// KLADDE_TRANSPARENT(Text, content)
/// ```
#define KLADDE_TRANSPARENT(Type, member)                                  \
    constexpr auto kladde_transparent(::kladde::type_t<Type>) { return &Type::member; } \
    constexpr std::string_view kladde_name(::kladde::type_t<Type>) { return #Type; }

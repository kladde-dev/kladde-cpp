// The canonical encoding of a descriptor table (spec/schema/canonical-encoding),
// and the layout of a descriptor that it shares with the fingerprint.
#include "internal.hpp"

#include <kladde/varint.hpp>

#include <algorithm>
#include <limits>

namespace kladde::schema {

namespace detail {

std::vector<Variant const*> canonical_variants(std::vector<Variant> const& variants) {
    std::vector<Variant const*> ordered;
    ordered.reserve(variants.size());
    for (auto const& v : variants) ordered.push_back(&v);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](Variant const* a, Variant const* b) { return a->discriminant < b->discriminant; });
    return ordered;
}

namespace {

void put_string(std::vector<std::uint8_t>& out, std::string const& s) {
    put_varint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

struct LayoutWriter {
    Layout& l;
    LayoutFor purpose;

    void tag(std::uint8_t t) { l.bytes.push_back(t); }
    void varint(std::uint64_t v) { put_varint(l.bytes, v); }
    void string(std::string const& s) { put_string(l.bytes, s); }
    void ref(TypeRef r) { l.slots.push_back({l.bytes.size(), r}); }
    // A Struct's and an Enum's own name is diagnostics only, and is left out
    // of the fingerprint so that renaming a type keeps it.
    void name(std::string const& s) {
        if (purpose == LayoutFor::encoding) string(s);
    }

    void operator()(Primitive p) { tag(static_cast<std::uint8_t>(p)); }
    void operator()(Struct const& s) {
        tag(tag_struct);
        name(s.name);
        varint(s.fields.size());
        for (auto const& f : s.fields) {
            string(f.name);
            ref(f.type);
        }
    }
    void operator()(Enum const& e) {
        tag(tag_enum);
        name(e.name);
        l.bytes.push_back(e.discriminant_width);
        varint(e.variants.size());
        for (auto const* v : canonical_variants(e.variants)) {
            varint(v->discriminant);
            string(v->name);
            ref(v->payload);
        }
    }
    void operator()(Opaque const& o) {
        tag(tag_opaque);
        string(o.library_name);
        string(o.type_name);
        if (purpose == LayoutFor::encoding) {
            varint(o.version.major);
            varint(o.version.minor);
            varint(o.version.patch);
        } else {
            // The compatibility component: whether the version is past 0.x,
            // and its leading nonzero component, so that 0.1.z and 1.y.z
            // differ.
            bool stable = o.version.major > 0;
            l.bytes.push_back(stable ? 1 : 0);
            varint(stable ? o.version.major : o.version.minor);
        }
        // Wraps to 0 for a fixed size of 2^64 - 1, which `validate` refuses.
        varint(o.fixed_size ? *o.fixed_size + 1 : 0);
        varint(o.parameters.size());
        for (auto r : o.parameters) ref(r);
    }
    void operator()(Array const& a) {
        tag(tag_array);
        ref(a.element);
        varint(a.length);
    }
    void operator()(Sequence const& s) {
        tag(tag_sequence);
        ref(s.element);
    }
    void operator()(Map const& m) {
        tag(tag_map);
        ref(m.key);
        ref(m.value);
    }
    void operator()(Text const&) { tag(tag_text); }
    void operator()(Fixed const& f) {
        tag(tag_fixed);
        ref(f.inner);
    }
};

}  // namespace

Layout layout(TypeDescriptor const& d, LayoutFor purpose) {
    Layout l;
    std::visit(LayoutWriter{l, purpose}, d);
    return l;
}

std::vector<TypeRef> references(TypeDescriptor const& d) {
    struct Visitor {
        std::vector<TypeRef> operator()(Primitive) const { return {}; }
        std::vector<TypeRef> operator()(Text const&) const { return {}; }
        std::vector<TypeRef> operator()(Struct const& s) const {
            std::vector<TypeRef> out;
            for (auto const& f : s.fields) out.push_back(f.type);
            return out;
        }
        std::vector<TypeRef> operator()(Enum const& e) const {
            std::vector<TypeRef> out;
            for (auto const* v : canonical_variants(e.variants)) out.push_back(v->payload);
            return out;
        }
        std::vector<TypeRef> operator()(Opaque const& o) const { return o.parameters; }
        std::vector<TypeRef> operator()(Array const& a) const { return {a.element}; }
        std::vector<TypeRef> operator()(Sequence const& s) const { return {s.element}; }
        std::vector<TypeRef> operator()(Map const& m) const { return {m.key, m.value}; }
        std::vector<TypeRef> operator()(Fixed const& f) const { return {f.inner}; }
    };
    return std::visit(Visitor{}, d);
}

bool valid_utf8(std::span<std::uint8_t const> bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        std::uint8_t b = bytes[i];
        if (b < 0x80) {
            ++i;
            continue;
        }
        std::size_t n;
        std::uint8_t lo = 0x80, hi = 0xbf;  // the range of the second byte
        if (b >= 0xc2 && b <= 0xdf) {
            n = 2;
        } else if (b >= 0xe0 && b <= 0xef) {
            n = 3;
            if (b == 0xe0) lo = 0xa0;       // no overlong forms
            if (b == 0xed) hi = 0x9f;       // no surrogates
        } else if (b >= 0xf0 && b <= 0xf4) {
            n = 4;
            if (b == 0xf0) lo = 0x90;       // no overlong forms
            if (b == 0xf4) hi = 0x8f;       // nothing above U+10FFFF
        } else {
            return false;
        }
        if (bytes.size() - i < n) return false;
        if (bytes[i + 1] < lo || bytes[i + 1] > hi) return false;
        for (std::size_t k = 2; k < n; ++k) {
            if ((bytes[i + k] & 0xc0) != 0x80) return false;
        }
        i += n;
    }
    return true;
}

}  // namespace detail

std::vector<std::uint8_t> TypeTable::encode() const {
    std::vector<std::uint8_t> out;
    put_varint(out, descriptors_.size());
    for (auto const& d : descriptors_) {
        auto l = detail::layout(d, detail::LayoutFor::encoding);
        std::size_t from = 0;
        for (auto const& slot : l.slots) {
            out.insert(out.end(), l.bytes.begin() + std::ptrdiff_t(from), l.bytes.begin() + std::ptrdiff_t(slot.at));
            put_varint(out, slot.ref.index);
            from = slot.at;
        }
        out.insert(out.end(), l.bytes.begin() + std::ptrdiff_t(from), l.bytes.end());
    }
    return out;
}

namespace {

// What is wrong, without saying where: `decode` adds that.
kladde::error corrupt(std::string message) { return {errc::corrupt, std::move(message)}; }

kladde::error located(kladde::error e, std::string const& where) {
    e.message = "schema: " + where + e.message;
    return e;
}

class Reader {
public:
    explicit Reader(std::span<std::uint8_t const> in) : in_(in) {}

    bool empty() const { return in_.empty(); }
    std::size_t remaining() const { return in_.size(); }

    // The canonical encoding writes every varint minimally, so a reader
    // refuses a longer one: a table has exactly one encoding.
    expected<std::uint64_t> varint() {
        if (auto v = get_varint(in_, true)) return *v;
        auto copy = in_;
        if (get_varint(copy, false)) return corrupt("a varint is longer than its value needs");
        return corrupt(in_.size() < 10 ? "truncated" : "a varint overflows 64 bits");
    }

    expected<std::uint8_t> byte() {
        if (in_.empty()) return corrupt("truncated");
        std::uint8_t b = in_.front();
        in_ = in_.subspan(1);
        return b;
    }

    expected<std::string> string() {
        KLADDE_TRY_ASSIGN(auto n, varint());
        if (n > in_.size()) return corrupt("truncated");
        auto bytes = in_.first(std::size_t(n));
        if (!detail::valid_utf8(bytes)) return corrupt("a string is not valid UTF-8");
        in_ = in_.subspan(std::size_t(n));
        return std::string(bytes.begin(), bytes.end());
    }

    expected<TypeRef> reference() {
        KLADDE_TRY_ASSIGN(auto i, varint());
        if (i > std::numeric_limits<std::size_t>::max()) return corrupt("a reference is out of range");
        return TypeRef{std::size_t(i)};
    }

    // A count of items that take at least one byte each: no larger than
    // what is left, so that reserving for it is safe.
    std::size_t capacity_for(std::uint64_t count) const {
        return std::size_t(std::min<std::uint64_t>(count, in_.size()));
    }

private:
    std::span<std::uint8_t const> in_;
};

expected<TypeDescriptor> decode_descriptor(Reader& r) {
    KLADDE_TRY_ASSIGN(auto tag, r.byte());
    if (tag < 128) {
        if (!detail::known_primitive(tag)) return corrupt("unsupported primitive code " + std::to_string(tag));
        return TypeDescriptor{static_cast<Primitive>(tag)};
    }
    switch (tag) {
        case tag_struct: {
            Struct s;
            KLADDE_TRY_ASSIGN(s.name, r.string());
            KLADDE_TRY_ASSIGN(auto count, r.varint());
            s.fields.reserve(r.capacity_for(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                Field f;
                KLADDE_TRY_ASSIGN(f.name, r.string());
                KLADDE_TRY_ASSIGN(f.type, r.reference());
                s.fields.push_back(std::move(f));
            }
            return TypeDescriptor{std::move(s)};
        }
        case tag_enum: {
            Enum e;
            KLADDE_TRY_ASSIGN(e.name, r.string());
            KLADDE_TRY_ASSIGN(e.discriminant_width, r.byte());
            KLADDE_TRY_ASSIGN(auto count, r.varint());
            e.variants.reserve(r.capacity_for(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                Variant v;
                KLADDE_TRY_ASSIGN(v.discriminant, r.varint());
                KLADDE_TRY_ASSIGN(v.name, r.string());
                KLADDE_TRY_ASSIGN(v.payload, r.reference());
                // Canonical order is ascending; equal discriminants would
                // make two variants indistinguishable.
                if (!e.variants.empty() && v.discriminant <= e.variants.back().discriminant)
                    return corrupt("enum variants are not in strictly ascending discriminant order");
                e.variants.push_back(std::move(v));
            }
            return TypeDescriptor{std::move(e)};
        }
        case tag_opaque: {
            Opaque o;
            KLADDE_TRY_ASSIGN(o.library_name, r.string());
            KLADDE_TRY_ASSIGN(o.type_name, r.string());
            KLADDE_TRY_ASSIGN(o.version.major, r.varint());
            KLADDE_TRY_ASSIGN(o.version.minor, r.varint());
            KLADDE_TRY_ASSIGN(o.version.patch, r.varint());
            KLADDE_TRY_ASSIGN(auto size_mode, r.varint());
            if (size_mode > 0) o.fixed_size = size_mode - 1;
            KLADDE_TRY_ASSIGN(auto count, r.varint());
            o.parameters.reserve(r.capacity_for(count));
            for (std::uint64_t i = 0; i < count; ++i) {
                KLADDE_TRY_ASSIGN(auto p, r.reference());
                o.parameters.push_back(p);
            }
            return TypeDescriptor{std::move(o)};
        }
        case tag_array: {
            Array a;
            KLADDE_TRY_ASSIGN(a.element, r.reference());
            KLADDE_TRY_ASSIGN(a.length, r.varint());
            return TypeDescriptor{a};
        }
        case tag_map: {
            Map m;
            KLADDE_TRY_ASSIGN(m.key, r.reference());
            KLADDE_TRY_ASSIGN(m.value, r.reference());
            return TypeDescriptor{m};
        }
        case tag_sequence: {
            KLADDE_TRY_ASSIGN(auto element, r.reference());
            return TypeDescriptor{Sequence{element}};
        }
        case tag_text: return TypeDescriptor{Text{}};
        case tag_fixed: {
            KLADDE_TRY_ASSIGN(auto inner, r.reference());
            return TypeDescriptor{Fixed{inner}};
        }
        default: return corrupt("unknown kind tag " + std::to_string(tag));
    }
}

}  // namespace

expected<TypeTable> TypeTable::decode(std::span<std::uint8_t const> bytes) {
    Reader r(bytes);
    auto count = r.varint();
    if (!count) return located(std::move(count).error(), "descriptor count: ");
    if (*count == 0) return located(corrupt("a table has no descriptors"), "");
    std::vector<TypeDescriptor> descriptors;
    descriptors.reserve(r.capacity_for(*count));
    for (std::uint64_t i = 0; i < *count; ++i) {
        auto d = decode_descriptor(r);
        if (!d) return located(std::move(d).error(), "descriptor " + std::to_string(i) + ": ");
        descriptors.push_back(std::move(*d));
    }
    if (!r.empty()) return located(corrupt("bytes follow the last descriptor"), "");
    TypeTable table(std::move(descriptors));
    KLADDE_TRY(table.validate());
    return table;
}

}  // namespace kladde::schema

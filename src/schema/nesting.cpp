// What follows from a descriptor table without a value: which types are
// fixed-size, how large a fixed form is, the rules a table keeps
// (spec/schema/type-descriptors#rules-of-nesting), and how long the encoding
// of a value at the front of some bytes is.
#include "internal.hpp"

#include <kladde/varint.hpp>

#include <algorithm>
#include <limits>

namespace kladde::schema {

namespace {

using Size = std::optional<std::uint64_t>;

Size checked_add(Size a, Size b) {
    if (!a || !b || *a > std::numeric_limits<std::uint64_t>::max() - *b) return std::nullopt;
    return *a + *b;
}

Size checked_mul(Size a, std::uint64_t b) {
    if (!a || (b != 0 && *a > std::numeric_limits<std::uint64_t>::max() / b)) return std::nullopt;
    return *a * b;
}

// Sizes of encodings (`form` false) and of fixed forms (`form` true),
// memoized per type and form. A type met again while it is being sized
// reaches itself, so neither it nor anything that holds it has a size.
//
// The two forms keep separate memos: a type can be met both inside and
// outside a `Fixed`, and its sizes in the two differ (a `u32` has a fixed
// form of 4 bytes but no fixed size).
//
// The search keeps an explicit stack rather than recursing, so that a long
// chain of descriptors in a decoded table cannot overflow the call stack.
class Sizer {
public:
    explicit Sizer(TypeTable const& table)
        : table_(table), entries_(2 * table.descriptors().size()) {}

    Size size(std::size_t node, bool form) {
        if (entry(node, form).state == State::known) return entry(node, form).value;
        std::vector<Key> stack{{node, form}};
        std::vector<Key> children;
        while (!stack.empty()) {
            auto [n, f] = stack.back();
            auto& e = entry(n, f);
            if (e.state == State::known) {
                stack.pop_back();
            } else if (e.state == State::unknown) {
                // Its children first; it is computed when it is on top again.
                e.state = State::in_progress;
                children.clear();
                children_of(n, f, children);
                for (auto c : children) {
                    if (entry(c.node, c.form).state == State::unknown) stack.push_back(c);
                }
            } else {
                e.value = compute(n, f);
                e.state = State::known;
                stack.pop_back();
            }
        }
        return entry(node, form).value;
    }

private:
    enum class State : std::uint8_t { unknown, in_progress, known };
    struct Entry {
        State state = State::unknown;
        Size value;
    };
    struct Key {
        std::size_t node;
        bool form;
    };

    Entry& entry(std::size_t node, bool form) { return entries_.at(2 * node + (form ? 1 : 0)); }

    // A child's size, once its children are done: none if it is still in
    // progress, which makes it part of a cycle.
    Size child(TypeRef r, bool form) {
        auto& e = entry(r.index, form);
        return e.state == State::known ? e.value : std::nullopt;
    }

    void children_of(std::size_t node, bool form, std::vector<Key>& out) const {
        auto const& d = table_.descriptors().at(node);
        if (auto const* f = std::get_if<Fixed>(&d)) {
            out.push_back({f->inner.index, true});
            return;
        }
        // An Opaque's parameters are not part of its encoding, and a
        // Sequence's, a Map's and Text's sizes never are fixed.
        if (std::holds_alternative<Opaque>(d) || std::holds_alternative<Sequence>(d) ||
            std::holds_alternative<Map>(d))
            return;
        for (auto r : detail::references(d)) out.push_back({r.index, form});
    }

    Size compute(std::size_t node, bool form) {
        auto const& d = table_.descriptors()[node];
        if (auto const* p = std::get_if<Primitive>(&d)) {
            if (!detail::known_primitive(static_cast<std::uint8_t>(*p))) return std::nullopt;
            return form ? Size(schema::fixed_form_size(*p)) : schema::fixed_size(*p);
        }
        if (auto const* s = std::get_if<Struct>(&d)) {
            Size sum = 0;
            for (auto const& f : s->fields) sum = checked_add(sum, child(f.type, form));
            return sum;
        }
        if (auto const* e = std::get_if<Enum>(&d)) {
            if (e->variants.empty()) return std::nullopt;
            Size largest = 0;
            bool same = true, one_byte = true;
            Size first = child(e->variants.front().payload, form);
            for (auto const& v : e->variants) {
                auto s = child(v.payload, form);
                if (!s) return std::nullopt;
                largest = std::max(*largest, *s);
                same = same && s == first;
                one_byte = one_byte && v.discriminant < 128;
            }
            // The fixed form pads every variant to the largest; the encoding
            // is fixed-size only if the discriminant's varint takes one byte
            // and every payload takes the same.
            if (form) return checked_add(e->discriminant_width, largest);
            if (same && one_byte) return checked_add(1, first);
            return std::nullopt;
        }
        if (auto const* o = std::get_if<Opaque>(&d)) return o->fixed_size;
        if (auto const* a = std::get_if<Array>(&d)) return checked_mul(child(a->element, form), a->length);
        if (auto const* f = std::get_if<Fixed>(&d)) return child(f->inner, true);
        return std::nullopt;  // Sequence, Map, Text
    }

    TypeTable const& table_;
    std::vector<Entry> entries_;
};

kladde::error invalid(std::size_t index, std::string const& reason) {
    return {errc::corrupt, "schema: descriptor " + std::to_string(index) + ": " + reason};
}

// The children a value of a descriptor holds inline: for a type to reach
// itself through these alone would make its values infinite.
std::vector<TypeRef> inline_children(TypeDescriptor const& d) {
    if (std::holds_alternative<Struct>(d) || std::holds_alternative<Array>(d) ||
        std::holds_alternative<Fixed>(d))
        return detail::references(d);
    return {};
}

expected<void> no_endless_cycle(TypeTable const& table) {
    enum class Mark : std::uint8_t { unvisited, in_progress, done };
    auto const& ds = table.descriptors();
    std::vector<Mark> marks(ds.size(), Mark::unvisited);
    struct Frame {
        std::size_t node;
        std::vector<TypeRef> children;
        std::size_t next = 0;
    };
    std::vector<Frame> stack;
    for (std::size_t start = 0; start < ds.size(); ++start) {
        if (marks[start] != Mark::unvisited) continue;
        marks[start] = Mark::in_progress;
        stack.push_back({start, inline_children(ds[start])});
        while (!stack.empty()) {
            auto& top = stack.back();
            if (top.next == top.children.size()) {
                marks[top.node] = Mark::done;
                stack.pop_back();
                continue;
            }
            auto c = top.children[top.next++].index;
            if (marks[c] == Mark::in_progress)
                return invalid(c, "a type contains itself without an enum, a sequence or a map between");
            if (marks[c] == Mark::unvisited) {
                marks[c] = Mark::in_progress;
                stack.push_back({c, inline_children(ds[c])});
            }
        }
    }
    return {};
}

// The checks on one descriptor that need nothing but the descriptor and the
// table's size.
expected<void> check_descriptor(std::size_t index, TypeDescriptor const& d, std::size_t table_size) {
    for (auto r : detail::references(d)) {
        if (r.index >= table_size) {
            return invalid(index, "reference " + std::to_string(r.index) + " is out of range for a table of " +
                                      std::to_string(table_size));
        }
    }
    if (auto const* p = std::get_if<Primitive>(&d)) {
        auto code = static_cast<std::uint8_t>(*p);
        if (!detail::known_primitive(code)) return invalid(index, "unsupported primitive code " + std::to_string(code));
    }
    if (auto const* e = std::get_if<Enum>(&d)) {
        auto w = e->discriminant_width;
        if (w != 1 && w != 2 && w != 4 && w != 8)
            return invalid(index, "invalid discriminant width " + std::to_string(w));
        auto ordered = detail::canonical_variants(e->variants);
        for (std::size_t i = 0; i < ordered.size(); ++i) {
            if (w < 8 && ordered[i]->discriminant >> (8 * w) != 0)
                return invalid(index, "a discriminant does not fit the enum's discriminant width");
            if (i > 0 && ordered[i]->discriminant == ordered[i - 1]->discriminant)
                return invalid(index, "two variants share a discriminant");
        }
    }
    if (auto const* o = std::get_if<Opaque>(&d)) {
        if (o->fixed_size == std::numeric_limits<std::uint64_t>::max())
            return invalid(index, "an opaque type's size mode does not fit 64 bits");
    }
    return {};
}

}  // namespace

std::optional<std::uint64_t> TypeTable::fixed_size(TypeRef r) const { return Sizer(*this).size(r.index, false); }

std::optional<std::uint64_t> TypeTable::fixed_form_size(TypeRef r) const {
    return Sizer(*this).size(r.index, true);
}

expected<void> TypeTable::validate() const {
    if (descriptors_.empty()) return kladde::error{errc::corrupt, "schema: a table has no descriptors"};
    for (std::size_t i = 0; i < descriptors_.size(); ++i) KLADDE_TRY(check_descriptor(i, descriptors_[i], descriptors_.size()));
    Sizer sizer(*this);
    for (std::size_t i = 0; i < descriptors_.size(); ++i) {
        auto const* f = std::get_if<Fixed>(&descriptors_[i]);
        if (f && !sizer.size(f->inner.index, true))
            return invalid(i, "a Fixed wrapper holds a type without a fixed form");
    }
    return no_endless_cycle(*this);
}

namespace {

// Walks one value's encoding, as `TypeTable::measure` describes.
class Measure {
public:
    Measure(TypeTable const& table, std::span<std::uint8_t const> bytes)
        : table_(table), bytes_(bytes), sizer_(table) {}

    std::optional<std::size_t> run(std::size_t node) {
        if (!skip(node, false, 0)) return std::nullopt;
        return at_;
    }

private:
    std::optional<std::span<std::uint8_t const>> take(std::uint64_t n) {
        if (n > bytes_.size() - at_) return std::nullopt;
        auto out = bytes_.subspan(at_, std::size_t(n));
        at_ += std::size_t(n);
        return out;
    }

    std::optional<std::uint64_t> little_endian(std::size_t n) {
        auto raw = take(n);
        if (!raw) return std::nullopt;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v |= std::uint64_t((*raw)[i]) << (8 * i);
        return v;
    }

    std::optional<std::uint64_t> varint() {
        auto rest = bytes_.subspan(at_);
        auto v = get_varint(rest, true);
        if (v) at_ = bytes_.size() - rest.size();
        return v;
    }

    bool zero_sized(TypeRef r, bool form) { return sizer_.size(r.index, form) == std::uint64_t{0}; }

    bool skip_primitive(Primitive p, bool form) {
        if (form) {
            auto v = little_endian(std::size_t(schema::fixed_form_size(p)));
            if (!v) return false;
            if (p == Primitive::boolean) return *v <= 1;
            if (p == Primitive::character) return *v <= 0x10ffff && !(*v >= 0xd800 && *v <= 0xdfff);
            return true;
        }
        std::uint64_t max = 0;
        switch (p) {
            case Primitive::u8:
            case Primitive::i8: return take(1).has_value();
            case Primitive::boolean: {
                auto b = take(1);
                return b && (*b)[0] <= 1;
            }
            case Primitive::f32: return take(4).has_value();
            case Primitive::f64: return take(8).has_value();
            case Primitive::character: {
                if (at_ == bytes_.size()) return false;
                auto lead = bytes_[at_];
                std::size_t n = lead < 0x80 ? 1 : lead < 0xc0 ? 0 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
                if (n == 0) return false;
                auto raw = take(n);
                return raw && detail::valid_utf8(*raw);
            }
            case Primitive::u16:
            case Primitive::i16: max = 0xffff; break;
            case Primitive::u32:
            case Primitive::i32: max = 0xffffffff; break;
            case Primitive::u64:
            case Primitive::i64: max = std::numeric_limits<std::uint64_t>::max(); break;
        }
        auto v = varint();
        return v && *v <= max;
    }

    bool skip(std::size_t node, bool form, std::size_t depth) {
        if (depth > TypeTable::max_measure_depth) return false;
        ++depth;
        auto const& d = table_.descriptors().at(node);
        if (auto const* p = std::get_if<Primitive>(&d)) return skip_primitive(*p, form);
        if (auto const* s = std::get_if<Struct>(&d)) {
            for (auto const& f : s->fields) {
                if (!skip(f.type.index, form, depth)) return false;
            }
            return true;
        }
        if (auto const* e = std::get_if<Enum>(&d)) {
            auto start = at_;
            auto discriminant = form ? little_endian(e->discriminant_width) : varint();
            if (!discriminant) return false;
            auto v = std::find_if(e->variants.begin(), e->variants.end(),
                                  [&](Variant const& x) { return x.discriminant == *discriminant; });
            if (v == e->variants.end() || !skip(v->payload.index, form, depth)) return false;
            if (!form) return true;
            auto size = sizer_.size(node, true);
            if (!size || *size < at_ - start) return false;
            auto padding = take(*size - (at_ - start));
            return padding && std::all_of(padding->begin(), padding->end(), [](auto b) { return b == 0; });
        }
        if (auto const* o = std::get_if<Opaque>(&d)) {
            auto n = o->fixed_size ? o->fixed_size : varint();
            return n && take(*n);
        }
        if (auto const* a = std::get_if<Array>(&d)) {
            // Values of a zero-sized type take no bytes and have nothing to
            // check; skipping them one by one could take 2^64 steps.
            if (zero_sized(a->element, form)) return true;
            for (std::uint64_t i = 0; i < a->length; ++i) {
                if (!skip(a->element.index, form, depth)) return false;
            }
            return true;
        }
        if (auto const* f = std::get_if<Fixed>(&d)) return skip(f->inner.index, true, depth);
        if (form) return false;  // Sequence, Map and Text have no fixed form
        if (auto const* s = std::get_if<Sequence>(&d)) {
            auto count = varint();
            if (!count) return false;
            if (zero_sized(s->element, false)) return true;
            for (std::uint64_t i = 0; i < *count; ++i) {
                if (!skip(s->element.index, false, depth)) return false;
            }
            return true;
        }
        if (auto const* m = std::get_if<Map>(&d)) {
            auto count = varint();
            if (!count) return false;
            if (zero_sized(m->key, false) && zero_sized(m->value, false)) return true;
            for (std::uint64_t i = 0; i < *count; ++i) {
                if (!skip(m->key.index, false, depth) || !skip(m->value.index, false, depth)) return false;
            }
            return true;
        }
        // Text: UTF-8 up to the terminator 0xFF, which UTF-8 never contains.
        auto rest = bytes_.subspan(at_);
        auto end = std::find(rest.begin(), rest.end(), std::uint8_t{0xff});
        if (end == rest.end()) return false;
        auto text = rest.first(std::size_t(end - rest.begin()));
        if (!detail::valid_utf8(text)) return false;
        at_ += text.size() + 1;
        return true;
    }

    TypeTable const& table_;
    std::span<std::uint8_t const> bytes_;
    std::size_t at_ = 0;
    Sizer sizer_;
};

}  // namespace

std::optional<std::size_t> TypeTable::measure(TypeRef r, std::span<std::uint8_t const> bytes) const {
    return Measure(*this, bytes).run(r.index);
}

}  // namespace kladde::schema

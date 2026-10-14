// `kladde::List<T>`: elements at stable addresses, each of which knows its
// list and its place in it, so that a `kladde::Ref` to an element stays valid
// however the rest of the document changes, until the element is removed.
#pragma once

#include <kladde/typed/vector.hpp>

namespace kladde {

template <class T>
class List;
template <class T>
class ListMut;
template <class T>
class Ref;

/// What a `List` has in common whatever it holds: the anchor of the value
/// that holds it, from which it finds its own start.
class ListBase {
public:
    /// Whether the list is part of a file's value: it has an owner, and
    /// through it a start in the sequence.
    bool attached() const { return owner_ != nullptr; }

protected:
    std::uint64_t list_start() const {
        std::uint64_t offset = 0;
        [[maybe_unused]] bool found = owner_->locate(this, offset);
        assert(found && "a list lies inside the value of its owner");
        return owner_->start() + offset;
    }
    Anchor* owner_ = nullptr;
};

/// An element of a `kladde::List`: the value, at an address that does not
/// change while it is in the list, nor when it is removed and inserted again.
template <class T>
class ListElement final : public Anchor {
public:
    explicit ListElement(T v) : value(std::move(v)) {}
    T value;

    /// The list the element is in, or null if it is in none.
    List<T>* list() const { return list_; }
    std::size_t index() const { return index_; }

    std::uint64_t start() const override { return list_->start() + list_->offset_of(index_); }
    void grew(std::int64_t delta) override { list_->element_grew(index_, delta); }
    Backend& backend() const override { return list_->backend(); }
    bool locate(void const* target, std::uint64_t& offset) const override {
        return kladde::locate(value, target, offset);
    }

private:
    friend class List<T>;
    friend class ListMut<T>;
    List<T>* list_ = nullptr;
    std::size_t index_ = 0;
};

/// A list of durable values whose elements stay where they are in memory:
/// the counterpart of `std::list` to `kladde::Vector`'s `std::vector`. On file
/// it is a Sequence, exactly as a `kladde::Vector` of the same elements.
///
/// Each element keeps its list and its index, and the list keeps a Fenwick
/// tree of its elements' sizes, about 16 bytes per element beyond the value.
/// A `kladde::Ref` to an element then finds where it starts whenever it
/// writes, by climbing from the element to the root, in O(depth · log m).
/// Inserting or removing an element renumbers the elements after it, in
/// O(m).
///
/// A `List` may be nested in structs, variants, options and boxes, and in
/// the elements of other lists, but not in a `kladde::Vector`, a
/// `std::vector` or a `kladde::Map`, whose indexes would not hear of its
/// changes through a `Ref`.
///
/// ```cpp
/// struct Element;
/// using Node = std::variant<kladde::Box<Element>, Text>;
/// struct Element { std::string name; kladde::List<Node> children; };
/// KLADDE_STRUCT(Element, name, children)
///
/// auto root = file.edit();
/// root.children().push_back(Text{"hello"});
/// kladde::Ref<Node> hello = file.get().children.ref(0);
/// // ... any number of edits elsewhere ...
/// hello.edit().visit([](auto alt) { /* write through it */ });
/// ```
template <class T>
class List : public ListBase {
public:
    using element = ListElement<T>;

    List() = default;
    List(std::initializer_list<T> items) {
        for (auto& x : items) push_memory(std::make_unique<element>(x));
        reindex();
    }
    explicit List(std::vector<T> items) {
        for (auto& x : items) push_memory(std::make_unique<element>(std::move(x)));
        reindex();
    }
    List(List const& o) : ListBase() {
        for (auto& e : o.elements_) push_memory(std::make_unique<element>(e->value));
        reindex();
    }
    List(List&& o) noexcept : ListBase(), elements_(std::move(o.elements_)), sizes_(std::move(o.sizes_)) {
        renumber(0);
    }
    List& operator=(List const& o) {
        List copy(o);
        *this = std::move(copy);
        return *this;
    }
    List& operator=(List&& o) noexcept {
        elements_ = std::move(o.elements_);
        sizes_ = std::move(o.sizes_);
        renumber(0);
        return *this;
    }

    std::size_t size() const { return elements_.size(); }
    bool empty() const { return elements_.empty(); }
    T const& operator[](std::size_t i) const { return elements_[i]->value; }
    /// The element at `i` itself, at its stable address.
    element* element_at(std::size_t i) const { return elements_[i].get(); }
    /// A long-lived handle to element `i`.
    Ref<T> ref(std::size_t i) const { return Ref<T>(elements_[i].get()); }
    friend bool operator==(List const& a, List const& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!(a[i] == b[i])) return false;
        return true;
    }

    class const_iterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        const_iterator() = default;
        explicit const_iterator(typename std::vector<std::unique_ptr<element>>::const_iterator it) : it_(it) {}
        T const& operator*() const { return (*it_)->value; }
        T const* operator->() const { return &(*it_)->value; }
        const_iterator& operator++() {
            ++it_;
            return *this;
        }
        const_iterator operator++(int) {
            auto c = *this;
            ++it_;
            return c;
        }
        friend bool operator==(const_iterator const&, const_iterator const&) = default;

    private:
        typename std::vector<std::unique_ptr<element>>::const_iterator it_;
    };
    const_iterator begin() const { return const_iterator(elements_.begin()); }
    const_iterator end() const { return const_iterator(elements_.end()); }

    /// Checks the index, the sizes and the elements' back links, aborting if
    /// one is wrong: for tests.
    void check() const {
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < elements_.size(); ++i) {
            if (elements_[i]->list_ != this || elements_[i]->index_ != i) std::abort();
            if (sizes_.prefix(i) != sum) std::abort();
            sum += durable<T>::encoded_len(elements_[i]->value);
        }
        if (sizes_.total() != sum || sizes_.base() != varint_len(elements_.size())) std::abort();
    }

private:
    friend class ListElement<T>;
    friend class ListMut<T>;
    friend struct durable<List<T>>;

    std::uint64_t start() const { return list_start(); }
    std::uint64_t offset_of(std::size_t i) const { return varint_len(elements_.size()) + sizes_.prefix(i); }
    Backend& backend() const { return owner_->backend(); }
    void element_grew(std::size_t i, std::int64_t delta) {
        sizes_.add(i, delta);
        owner_->grew(delta);
    }
    void push_memory(std::unique_ptr<element> e) {
        e->list_ = this;
        e->index_ = elements_.size();
        elements_.push_back(std::move(e));
    }
    void renumber(std::size_t from) {
        for (std::size_t i = from; i < elements_.size(); ++i) {
            elements_[i]->list_ = this;
            elements_[i]->index_ = i;
        }
    }
    void reindex() {
        std::vector<std::uint64_t> s;
        s.reserve(elements_.size());
        for (auto& e : elements_) s.push_back(durable<T>::encoded_len(e->value));
        sizes_ = Sizes::build(s.begin(), s.end(), varint_len(elements_.size()));
    }
    void attach_all(Anchor* owner) {
        owner_ = owner;
        renumber(0);
        for (auto& e : elements_) kladde::attach(e->value, e.get());
    }

    std::vector<std::unique_ptr<element>> elements_;
    Sizes sizes_;
};

/// A long-lived handle to an element of a `kladde::List`: valid until the
/// element is destroyed, as an iterator of `std::list` is, however the rest
/// of the document changes meanwhile. An element removed from its list and
/// kept (`ListMut::detach`) keeps its address, so a `Ref` to it stays valid
/// but has no list until the element is inserted again.
///
/// `edit()` makes a short-lived handle that finds where the element starts
/// when it writes, by climbing to the root.
template <class T>
class Ref {
public:
    Ref() = default;
    explicit Ref(ListElement<T>* e) : e_(e) {}

    /// Whether the element is in a list that is part of a file's value, so
    /// that `edit()` may be called.
    bool attached() const { return e_ && e_->list() && e_->list()->attached(); }
    T const& get() const { return e_->value; }
    T const& operator*() const { return e_->value; }
    T const* operator->() const { return &e_->value; }
    ListElement<T>* element() const { return e_; }
    std::size_t index() const { return e_->index(); }

    /// A handle to the element's value. Valid until something is written
    /// that is not through it or a handle made from it.
    Mut<T> edit() const {
        assert(attached() && "only an element of a file's value can be edited through the file");
        auto len = durable<T>::encoded_len(e_->value);
        return Mut<T>(e_->value, Region(e_->backend(), Link::at_anchor(e_, len), e_));
    }

    friend bool operator==(Ref const&, Ref const&) = default;

private:
    ListElement<T>* e_ = nullptr;
};

/// A list element cut out of its list, with the store's cut of its bytes:
/// pasted into a list while the journal that removed it lasts, it writes
/// none of its bytes.
template <class T>
struct ListCut {
    std::unique_ptr<ListElement<T>> element;
    std::optional<CutId> id;
};

/// The handle of a `kladde::List`: insert, remove, reorder and cut elements,
/// and reach each element's own handle.
template <class T>
class ListMut : public MutBase<List<T>> {
    using L = List<T>;
    using E = ListElement<T>;
    using MutBase<L>::value_;
    using MutBase<L>::region_;

public:
    using MutBase<L>::MutBase;

    std::size_t size() const { return value_->size(); }

    /// The handle of element `i`.
    Mut<T> at(std::size_t i) {
        assert(i < value_->size());
        auto* e = value_->elements_[i].get();
        return Mut<T>(e->value, region_.child(value_->offset_of(i), durable<T>::encoded_len(e->value),
                                             &value_->sizes_, i, e));
    }
    Mut<T> operator[](std::size_t i) { return at(i); }

    /// Inserts `value` before element `i`; returns the new element.
    E* insert(std::size_t i, T value) { return insert_element(i, std::make_unique<E>(std::move(value))); }
    E* push_back(T value) { return insert(value_->size(), std::move(value)); }

    /// Inserts `element`, which is in no list, before element `i`: its value
    /// is encoded and recorded, and the element, at its unchanged address,
    /// joins the list.
    E* insert_element(std::size_t i, std::unique_ptr<E> element) {
        assert(element && !element->list());
        auto bytes = to_bytes(element->value);
        return place(
            i, std::move(element),
            [&](std::uint64_t at) -> expected<void> {
                auto r = region_.splice(at, 0, bytes);
                if (!r) return std::move(r).error();
                return {};
            },
            bytes.size());
    }

    /// Removes element `i` and hands it back, at its unchanged address, so
    /// that it can be inserted again.
    std::unique_ptr<E> detach(std::size_t i) {
        auto c = cut(i);
        if (!c) return nullptr;
        return std::move(c->element);
    }
    void erase(std::size_t i) { (void)detach(i); }
    void clear() { this->set(L()); }

    /// Moves element `from` to position `to`: one `Move`, which writes none
    /// of the element's bytes.
    void move(std::size_t from, std::size_t to) {
        auto n = value_->size();
        if (from >= n || to >= n) return report(error{errc::out_of_bounds});
        if (from == to) return;
        auto& es = value_->elements_;
        auto start = value_->offset_of(from);
        auto len = durable<T>::encoded_len(es[from]->value);
        auto dest = to < from ? value_->offset_of(to) : value_->offset_of(to + 1) - len;
        if (!check(region_.backend(), region_.move(start, len, dest))) return;
        auto e = std::move(es[from]);
        es.erase(es.begin() + static_cast<std::ptrdiff_t>(from));
        es.insert(es.begin() + static_cast<std::ptrdiff_t>(to), std::move(e));
        value_->renumber(std::min(from, to));
        value_->reindex();
    }

    /// Cuts element `i` out, for a paste elsewhere that writes none of its
    /// bytes while the journal that removed it lasts.
    std::optional<ListCut<T>> cut(std::size_t i) {
        auto n = value_->size();
        if (i >= n) {
            report(error{errc::out_of_bounds});
            return std::nullopt;
        }
        auto at = value_->offset_of(i);
        auto len = durable<T>::encoded_len(value_->elements_[i]->value);
        std::optional<CutId> id;
        auto r = region_.transaction([&]() -> expected<void> {
            KLADDE_TRY_ASSIGN(id, region_.splice(at, len, {}));
            return write_count(region_, n, n - 1);
        });
        if (!check(region_.backend(), r)) return std::nullopt;
        auto& es = value_->elements_;
        auto e = std::move(es[i]);
        es.erase(es.begin() + static_cast<std::ptrdiff_t>(i));
        e->list_ = nullptr;
        value_->renumber(i);
        if (i == n - 1) {
            value_->sizes_.pop(len);
            value_->sizes_.set_base(varint_len(n - 1));
        } else {
            value_->reindex();
        }
        return ListCut<T>{std::move(e), id};
    }

    /// Inserts the element `cut` holds before element `i`: a paste of its
    /// bytes while the journal that cut it lasts, a new encoding otherwise.
    E* insert_cut(std::size_t i, ListCut<T> cut) {
        auto bytes = to_bytes(cut.element->value);
        auto id = cut.id;
        return place(
            i, std::move(cut.element),
            [&](std::uint64_t at) -> expected<void> {
                if (id) return region_.paste(at, 0, *id, bytes);
                auto r = region_.splice(at, 0, bytes);
                if (!r) return std::move(r).error();
                return {};
            },
            bytes.size());
    }

private:
    template <class Record>
    E* place(std::size_t i, std::unique_ptr<E> element, Record&& record, std::size_t len) {
        auto n = value_->size();
        if (i > n) {
            report(error{errc::out_of_bounds});
            return nullptr;
        }
        auto at = value_->offset_of(i);
        auto& es = value_->elements_;
        es.reserve(n + 1);
        auto r = region_.transaction([&]() -> expected<void> {
            KLADDE_TRY(record(at));
            return write_count(region_, n, n + 1);
        });
        if (!check(region_.backend(), r)) return nullptr;
        auto* e = element.get();
        es.insert(es.begin() + static_cast<std::ptrdiff_t>(i), std::move(element));
        value_->renumber(i);
        if (i == n) {
            value_->sizes_.push(len);
            value_->sizes_.set_base(varint_len(n + 1));
        } else {
            value_->reindex();
        }
        kladde::attach(e->value, e);
        return e;
    }
};

template <Durable T>
struct durable<List<T>> {
    using L = List<T>;
    static constexpr std::optional<std::size_t> fixed_size = std::nullopt;
    static constexpr bool contains_list = true;
    static std::size_t encoded_len(L const& l) {
        return varint_len(l.size()) + static_cast<std::size_t>(l.sizes_.total());
    }
    static void encode(L const& l, Encoder& out) {
        put_varint(out, l.size());
        for (auto& e : l.elements_) durable<T>::encode(e->value, out);
    }
    static bool decode(Decoder& in, L& l) {
        auto n = in.varint();
        if (!n) return false;
        L fresh;
        for (std::uint64_t i = 0; i < *n; ++i) {
            auto e = std::make_unique<ListElement<T>>(T{});
            if (!durable<T>::decode(in, e->value)) return false;
            fresh.push_memory(std::move(e));
        }
        fresh.reindex();
        l = std::move(fresh);
        return true;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        // A list is a vector on file: they share one descriptor.
        return b.describe(&type_key<Vector<T>>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            return schema::Sequence{durable<T>::describe(bb)};
        });
    }
    static bool locate(L const&, void const*, std::uint64_t&) {
        // The values inside a list's elements are found from the elements,
        // which are anchors of their own.
        return false;
    }
    static void attach(L& l, Anchor* anchor) { l.attach_all(anchor); }
    using mut = ListMut<T>;
};

}  // namespace kladde

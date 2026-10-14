// `kladde::Vector<T>`: a `std::vector<T>` and, for elements whose size
// depends on their value, a Fenwick tree of their sizes, so that element
// `i`'s handle finds where it starts in O(log m).
#pragma once

#include <kladde/typed/std_types.hpp>

namespace kladde {

/// A Fenwick tree over the encoded sizes of a container's elements, plus a
/// base: the bytes before the first element (the count). Element `i` starts
/// at `base + prefix(i)`.
class Sizes final : public Node {
public:
    Sizes() = default;
    template <class It>
    static Sizes build(It first, It last, std::uint64_t base) {
        Sizes s;
        s.base_ = base;
        for (; first != last; ++first) s.tree_.push_back(*first);
        for (auto t : s.tree_) s.total_ += t;
        for (std::size_t k = 1; k <= s.tree_.size(); ++k) {
            auto parent = k + lowbit(k);
            if (parent <= s.tree_.size()) s.tree_[parent - 1] += s.tree_[k - 1];
        }
        return s;
    }
    std::size_t size() const { return tree_.size(); }
    std::uint64_t total() const { return total_; }
    void set_base(std::uint64_t base) { base_ = base; }
    std::uint64_t base() const { return base_; }
    std::uint64_t prefix(std::size_t i) const {
        std::uint64_t sum = 0;
        for (auto k = i; k > 0; k -= lowbit(k)) sum += tree_[k - 1];
        return sum;
    }
    void add(std::size_t i, std::int64_t delta) {
        total_ = static_cast<std::uint64_t>(static_cast<std::int64_t>(total_) + delta);
        for (auto k = i + 1; k <= tree_.size(); k += lowbit(k))
            tree_[k - 1] = static_cast<std::uint64_t>(static_cast<std::int64_t>(tree_[k - 1]) + delta);
    }
    void push(std::uint64_t size) {
        auto k = tree_.size() + 1;
        auto covered = prefix(k - 1) - prefix(k - lowbit(k));
        total_ += size;
        tree_.push_back(covered + size);
    }
    void pop(std::uint64_t size) {
        tree_.pop_back();
        total_ -= size;
    }
    std::uint64_t offset_of(std::size_t index) const override { return base_ + prefix(index); }
    void resized(std::size_t index, std::uint64_t old_size, std::uint64_t new_size) override {
        add(index, static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
    }

private:
    static std::size_t lowbit(std::size_t k) { return k & (~k + 1); }
    std::vector<std::uint64_t> tree_;
    std::uint64_t total_ = 0;
    std::uint64_t base_ = 0;
};

/// Records the change of a container's count from `old_n` to `new_n` at the
/// start of `region`.
inline expected<void> write_count(Region& region, std::size_t old_n, std::size_t new_n) {
    Encoder b;
    put_varint(b, new_n);
    auto r = region.splice(0, varint_len(old_n), b);
    if (!r) return std::move(r).error();
    return {};
}

template <class T>
class VectorMut;

/// A vector of durable values, of any size: the number of elements as a
/// varint, then the elements.
///
/// In memory a `std::vector<T>`, and for a `T` whose size depends on its
/// value a Fenwick tree of the elements' sizes, so that the handle of element
/// `i` finds where it starts in O(log m). For a fixed-size `T` the element
/// starts at `i` times its size and the vector keeps no tree. Read it as a
/// `std::vector`; change it through its handle.
///
/// ```cpp
/// kladde::Vector<std::string> lines;
/// auto file = kladde::File<kladde::Vector<std::string>>::create("lines.kladde", lines).value();
/// file.edit().push_back("hello");
/// file.edit().at(0).append(", world");
/// assert(file.get()[0] == "hello, world");
/// ```
template <class T>
class Vector {
public:
    Vector() { reindex(); }
    Vector(std::initializer_list<T> items) : items_(items) { reindex(); }
    explicit Vector(std::vector<T> items) : items_(std::move(items)) { reindex(); }
    Vector(Vector const& o) : items_(o.items_), sizes_(o.sizes_) {}
    Vector(Vector&&) noexcept = default;
    Vector& operator=(Vector const& o) {
        items_ = o.items_;
        sizes_ = o.sizes_;
        return *this;
    }
    Vector& operator=(Vector&&) noexcept = default;

    std::vector<T> const& items() const { return items_; }
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    T const& operator[](std::size_t i) const { return items_[i]; }
    T const& at(std::size_t i) const { return items_.at(i); }
    T const& front() const { return items_.front(); }
    T const& back() const { return items_.back(); }
    auto begin() const { return items_.begin(); }
    auto end() const { return items_.end(); }
    friend bool operator==(Vector const& a, Vector const& b) { return a.items_ == b.items_; }

    /// Checks that the sizes the vector keeps match its elements, aborting
    /// otherwise: for tests.
    void check() const {
        if constexpr (!durable<T>::fixed_size) {
            assert(sizes_.size() == items_.size());
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (sizes_.prefix(i) != sum) std::abort();
                sum += durable<T>::encoded_len(items_[i]);
            }
            if (sizes_.total() != sum || sizes_.base() != varint_len(items_.size())) std::abort();
        }
    }

private:
    friend struct durable<Vector<T>>;
    friend class VectorMut<T>;

    std::uint64_t content_len() const {
        if constexpr (durable<T>::fixed_size) return *durable<T>::fixed_size * items_.size();
        else return sizes_.total();
    }
    std::uint64_t offset_of(std::size_t i) const {
        if constexpr (durable<T>::fixed_size) return varint_len(items_.size()) + i * *durable<T>::fixed_size;
        else return varint_len(items_.size()) + sizes_.prefix(i);
    }
    void reindex() {
        if constexpr (!durable<T>::fixed_size) {
            std::vector<std::uint64_t> s;
            s.reserve(items_.size());
            for (auto& x : items_) s.push_back(durable<T>::encoded_len(x));
            sizes_ = Sizes::build(s.begin(), s.end(), varint_len(items_.size()));
        }
    }

    std::vector<T> items_;
    Sizes sizes_;
};

/// The handle of a `kladde::Vector`: insert, remove, reorder and cut
/// elements, and reach each element's own handle.
///
/// An insertion or a removal is one transaction of two records, the element
/// and the count; a reordering is one `Move`, which writes none of the
/// element's bytes. Each changes memory only after the store recorded it.
template <class T>
class VectorMut : public MutBase<Vector<T>> {
    using MutBase<Vector<T>>::value_;
    using MutBase<Vector<T>>::region_;

public:
    using MutBase<Vector<T>>::MutBase;

    std::size_t size() const { return value_->size(); }

    /// The handle of element `i`, found in O(log m), or by a multiplication
    /// for a fixed-size `T`.
    Mut<T> at(std::size_t i) {
        assert(i < value_->size());
        auto& v = *value_;
        Node* node = durable<T>::fixed_size ? nullptr : &v.sizes_;
        return Mut<T>(v.items_[i], region_.child(v.offset_of(i), durable<T>::encoded_len(v.items_[i]), node, i));
    }
    Mut<T> operator[](std::size_t i) { return at(i); }

    /// Inserts `value` before element `i`, in O(m) in memory and two records
    /// in the file.
    void insert(std::size_t i, T value) {
        auto n = value_->size();
        if (i > n) return report(error{errc::out_of_bounds});
        auto at = value_->offset_of(i);
        auto bytes = to_bytes(value);
        value_->items_.reserve(n + 1);
        auto r = region_.transaction([&]() -> expected<void> {
            KLADDE_TRY(region_.splice(at, 0, bytes));
            return write_count(region_, n, n + 1);
        });
        if (!check(region_.backend(), r)) return;
        place(i, std::move(value), bytes.size());
    }
    void push_back(T value) { insert(value_->size(), std::move(value)); }

    /// Removes element `i` and returns it.
    std::optional<T> remove(std::size_t i) {
        auto n = value_->size();
        if (i >= n) {
            report(error{errc::out_of_bounds});
            return std::nullopt;
        }
        auto at = value_->offset_of(i);
        auto len = durable<T>::encoded_len(value_->items_[i]);
        auto r = region_.transaction([&]() -> expected<void> {
            KLADDE_TRY(region_.splice(at, len, {}));
            return write_count(region_, n, n - 1);
        });
        if (!check(region_.backend(), r)) return std::nullopt;
        return take(i);
    }
    void erase(std::size_t i) { (void)remove(i); }
    void pop_back() {
        if (!value_->empty()) erase(value_->size() - 1);
    }
    void clear() { this->set(Vector<T>()); }

    /// Moves element `from` to position `to`, shifting the elements between:
    /// one `Move` in the file, which writes none of the element's bytes.
    void move(std::size_t from, std::size_t to) {
        auto n = value_->size();
        if (from >= n || to >= n) return report(error{errc::out_of_bounds});
        if (from == to) return;
        auto start = value_->offset_of(from);
        auto len = durable<T>::encoded_len(value_->items_[from]);
        auto dest = to < from ? value_->offset_of(to) : value_->offset_of(to + 1) - len;
        if (!check(region_.backend(), region_.move(start, len, dest))) return;
        auto& items = value_->items_;
        auto item = std::move(items[from]);
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
        value_->reindex();
    }

    /// Cuts element `i` out, for a paste elsewhere without encoding it again
    /// as long as the journal that removed it lasts: see `kladde::Cut`.
    std::optional<Cut<T>> cut(std::size_t i) {
        auto n = value_->size();
        if (i >= n) {
            report(error{errc::out_of_bounds});
            return std::nullopt;
        }
        auto at = value_->offset_of(i);
        auto len = durable<T>::encoded_len(value_->items_[i]);
        std::optional<CutId> id;
        auto r = region_.transaction([&]() -> expected<void> {
            KLADDE_TRY_ASSIGN(id, region_.splice(at, len, {}));
            return write_count(region_, n, n - 1);
        });
        if (!check(region_.backend(), r)) return std::nullopt;
        return Cut<T>(take(i), id, region_.backend());
    }
    /// Inserts the value `cut` holds before element `i`: a paste of its bytes
    /// while the journal that cut it lasts, a new encoding otherwise.
    void insert_cut(std::size_t i, Cut<T> cut) {
        auto n = value_->size();
        if (i > n) return report(error{errc::out_of_bounds});
        auto at = value_->offset_of(i);
        auto bytes = to_bytes(*cut);
        auto id = cut.id();
        value_->items_.reserve(n + 1);
        auto r = region_.transaction([&]() -> expected<void> {
            if (id) {
                KLADDE_TRY(region_.paste(at, 0, *id, bytes));
            } else {
                KLADDE_TRY(region_.splice(at, 0, bytes));
            }
            return write_count(region_, n, n + 1);
        });
        if (!check(region_.backend(), r)) return;
        place(i, std::move(cut).take(), bytes.size());
    }
    void push_back_cut(Cut<T> cut) { insert_cut(value_->size(), std::move(cut)); }

private:
    void place(std::size_t i, T value, std::size_t len) {
        auto& v = *value_;
        auto n = v.items_.size();
        if (i == n && !durable<T>::fixed_size) {
            v.sizes_.push(len);
            v.sizes_.set_base(varint_len(n + 1));
            v.items_.push_back(std::move(value));
        } else {
            v.items_.insert(v.items_.begin() + static_cast<std::ptrdiff_t>(i), std::move(value));
            v.reindex();
        }
    }
    T take(std::size_t i) {
        auto& v = *value_;
        if (i + 1 == v.items_.size() && !durable<T>::fixed_size) {
            T value = std::move(v.items_.back());
            v.items_.pop_back();
            v.sizes_.pop(durable<T>::encoded_len(value));
            v.sizes_.set_base(varint_len(v.items_.size()));
            return value;
        }
        T value = std::move(v.items_[i]);
        v.items_.erase(v.items_.begin() + static_cast<std::ptrdiff_t>(i));
        v.reindex();
        return value;
    }
};

template <Durable T>
struct durable<Vector<T>> {
    static_assert(!contains_list_v<T>,
                  "a kladde::Vector cannot hold a kladde::List: use a kladde::List of them");
    static constexpr std::optional<std::size_t> fixed_size = std::nullopt;
    static std::size_t encoded_len(Vector<T> const& v) {
        return varint_len(v.size()) + static_cast<std::size_t>(v.content_len());
    }
    static void encode(Vector<T> const& v, Encoder& out) {
        put_varint(out, v.size());
        for (auto& x : v.items_) durable<T>::encode(x, out);
    }
    static bool decode(Decoder& in, Vector<T>& v) {
        auto n = in.varint();
        if (!n) return false;
        std::vector<T> items;
        items.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(*n, 1 << 16)));
        for (std::uint64_t i = 0; i < *n; ++i) {
            T x{};
            if (!durable<T>::decode(in, x)) return false;
            items.push_back(std::move(x));
        }
        v = Vector<T>(std::move(items));
        return true;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<Vector<T>>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            return schema::Sequence{durable<T>::describe(bb)};
        });
    }
    using mut = VectorMut<T>;
};

}  // namespace kladde

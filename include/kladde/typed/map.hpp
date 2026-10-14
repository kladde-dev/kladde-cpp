// `kladde::Map<K, V>`: an index map, the entries in a vector in their order on
// file and a hash table of their positions, so that each key is stored once.
#pragma once

#include <functional>
#include <unordered_set>

#include <kladde/typed/vector.hpp>

namespace kladde {

template <class K, class V>
class MapMut;

/// A map from durable keys to durable values: the number of entries as a
/// varint, then each entry's key and value, in the order the map keeps them.
///
/// A lookup is a hash and an index; an insertion appends; a removal moves the
/// last entry into the removed one's place, with a `Move` that writes none of
/// its bytes, so the file stays dense and the only entry whose position
/// changes is the moved one. Iteration follows the order on file.
///
/// ```cpp
/// kladde::File<kladde::Map<std::string, std::uint32_t>> loans = ...;
/// auto m = loans.edit();
/// m.insert("Emma", 3);
/// m.at("Emma")->set(4);
/// m.erase("Emma");
/// ```
template <class K, class V>
class Map {
public:
    using entry = std::pair<K, V>;

    Map() : index_(0, Hash{this}, Eq{this}) { reindex(); }
    Map(std::initializer_list<entry> es) : Map() {
        for (auto& e : es) insert_memory(e.first, e.second);
        reindex();
    }
    Map(Map const& o) : entries_(o.entries_), index_(0, Hash{this}, Eq{this}) { reindex(); }
    Map(Map&& o) noexcept : entries_(std::move(o.entries_)), index_(0, Hash{this}, Eq{this}) { reindex(); }
    Map& operator=(Map const& o) {
        entries_ = o.entries_;
        reindex();
        return *this;
    }
    Map& operator=(Map&& o) noexcept {
        entries_ = std::move(o.entries_);
        reindex();
        return *this;
    }

    std::size_t size() const { return entries_.size(); }
    bool empty() const { return entries_.empty(); }
    auto begin() const { return entries_.begin(); }
    auto end() const { return entries_.end(); }
    std::vector<entry> const& entries() const { return entries_; }
    /// The position of `key`'s entry on file, if the map holds it.
    std::optional<std::size_t> find(K const& key) const {
        auto it = index_.find(Probe{&key});
        if (it == index_.end()) return std::nullopt;
        return it->i;
    }
    bool contains(K const& key) const { return find(key).has_value(); }
    V const* get(K const& key) const {
        auto i = find(key);
        return i ? &entries_[*i].second : nullptr;
    }
    friend bool operator==(Map const& a, Map const& b) { return a.entries_ == b.entries_; }

private:
    friend struct durable<Map<K, V>>;
    friend class MapMut<K, V>;

    // The hash table holds positions; hashing and comparing a position looks
    // at the key of the entry there, and a lookup probes with a key.
    struct Slot {
        std::size_t i;
    };
    struct Probe {
        K const* key;
    };
    struct Hash {
        using is_transparent = void;
        Map const* m;
        std::size_t operator()(Slot s) const { return std::hash<K>{}(m->entries_[s.i].first); }
        std::size_t operator()(Probe p) const { return std::hash<K>{}(*p.key); }
    };
    struct Eq {
        using is_transparent = void;
        Map const* m;
        K const& key(Slot s) const { return m->entries_[s.i].first; }
        K const& key(Probe p) const { return *p.key; }
        template <class A, class B>
        bool operator()(A a, B b) const { return key(a) == key(b); }
    };

    static constexpr bool fixed = durable<K>::fixed_size && durable<V>::fixed_size;
    static std::uint64_t entry_len(entry const& e) {
        return durable<K>::encoded_len(e.first) + durable<V>::encoded_len(e.second);
    }
    std::uint64_t offset_of(std::size_t i) const {
        if constexpr (fixed)
            return varint_len(entries_.size()) + i * (*durable<K>::fixed_size + *durable<V>::fixed_size);
        else
            return varint_len(entries_.size()) + sizes_.prefix(i);
    }
    std::uint64_t content_len() const {
        if constexpr (fixed) return entries_.size() * (*durable<K>::fixed_size + *durable<V>::fixed_size);
        else return sizes_.total();
    }
    bool insert_memory(K const& k, V const& v) {
        if (contains(k)) return false;
        entries_.emplace_back(k, v);
        index_.insert(Slot{entries_.size() - 1});
        return true;
    }
    void reindex() {
        index_ = std::unordered_set<Slot, Hash, Eq>(entries_.size(), Hash{this}, Eq{this});
        for (std::size_t i = 0; i < entries_.size(); ++i) index_.insert(Slot{i});
        if constexpr (!fixed) {
            std::vector<std::uint64_t> s;
            for (auto& e : entries_) s.push_back(entry_len(e));
            sizes_ = Sizes::build(s.begin(), s.end(), varint_len(entries_.size()));
        }
    }

    std::vector<entry> entries_;
    std::unordered_set<Slot, Hash, Eq> index_;
    Sizes sizes_;
};

/// The handle of a `kladde::Map`: insert, erase, and reach each value's own
/// handle.
template <class K, class V>
class MapMut : public MutBase<Map<K, V>> {
    using M = Map<K, V>;
    using MutBase<M>::value_;
    using MutBase<M>::region_;

public:
    using MutBase<M>::MutBase;

    /// The handle of `key`'s value, if the map holds it.
    std::optional<Mut<V>> at(K const& key) {
        auto i = value_->find(key);
        if (!i) return std::nullopt;
        auto& e = value_->entries_[*i];
        auto at = value_->offset_of(*i) + durable<K>::encoded_len(e.first);
        Node* node = M::fixed ? nullptr : &value_->sizes_;
        // The value's size changes are its entry's: the node hears them at
        // the entry's index.
        return std::optional<Mut<V>>(std::in_place, e.second,
                                     region_.child(at, durable<V>::encoded_len(e.second), node, *i));
    }

    /// Sets `key`'s value: replaces it in place if the map holds the key, and
    /// appends an entry otherwise.
    void insert(K key, V value) {
        auto n = value_->size();
        if (auto i = value_->find(key)) {
            auto& e = value_->entries_[*i];
            auto at = value_->offset_of(*i) + durable<K>::encoded_len(e.first);
            auto old_len = durable<V>::encoded_len(e.second);
            auto bytes = to_bytes(value);
            if (!check(region_.backend(), region_.splice(at, old_len, bytes))) return;
            if constexpr (!M::fixed)
                value_->sizes_.add(*i, static_cast<std::int64_t>(bytes.size()) - static_cast<std::int64_t>(old_len));
            e.second = std::move(value);
            return;
        }
        auto at = value_->offset_of(n);
        auto bytes = to_bytes(key);
        durable<V>::encode(value, bytes);
        value_->entries_.reserve(n + 1);
        auto r = region_.transaction([&]() -> expected<void> {
            KLADDE_TRY(region_.splice(at, 0, bytes));
            return write_count(region_, n, n + 1);
        });
        if (!check(region_.backend(), r)) return;
        value_->entries_.emplace_back(std::move(key), std::move(value));
        value_->index_.insert(typename M::Slot{n});
        if constexpr (!M::fixed) {
            value_->sizes_.push(bytes.size());
            value_->sizes_.set_base(varint_len(n + 1));
        }
    }

    /// Removes `key`'s entry and returns its value: the last entry moves
    /// into its place, with a `Move` that writes none of its bytes.
    std::optional<V> remove(K const& key) {
        auto found = value_->find(key);
        if (!found) return std::nullopt;
        auto i = *found;
        auto n = value_->size();
        auto last = n - 1;
        auto at = value_->offset_of(i);
        auto len = M::entry_len(value_->entries_[i]);
        auto last_at = value_->offset_of(last);
        auto last_len = M::entry_len(value_->entries_[last]);
        auto r = region_.transaction([&]() -> expected<void> {
            if (i != last) KLADDE_TRY(region_.move(last_at, last_len, at));
            KLADDE_TRY(region_.splice(at + (i != last ? last_len : 0), len, {}));
            return write_count(region_, n, n - 1);
        });
        if (!check(region_.backend(), r)) return std::nullopt;
        auto& es = value_->entries_;
        V out = std::move(es[i].second);
        if (i != last) es[i] = std::move(es[last]);
        es.pop_back();
        value_->reindex();
        return out;
    }
    bool erase(K const& key) { return remove(key).has_value(); }
    void clear() { this->set(M()); }
};

template <Durable K, Durable V>
struct durable<Map<K, V>> {
    static_assert(!contains_list_v<K> && !contains_list_v<V>, "a kladde::Map cannot hold a kladde::List");
    using M = Map<K, V>;
    static constexpr std::optional<std::size_t> fixed_size = std::nullopt;
    static std::size_t encoded_len(M const& m) {
        return varint_len(m.size()) + static_cast<std::size_t>(m.content_len());
    }
    static void encode(M const& m, Encoder& out) {
        put_varint(out, m.size());
        for (auto& [k, v] : m.entries_) {
            durable<K>::encode(k, out);
            durable<V>::encode(v, out);
        }
    }
    static bool decode(Decoder& in, M& m) {
        auto n = in.varint();
        if (!n) return false;
        M fresh;
        for (std::uint64_t i = 0; i < *n; ++i) {
            K k{};
            V v{};
            if (!durable<K>::decode(in, k) || !durable<V>::decode(in, v)) return false;
            if (!fresh.insert_memory(k, v)) return in.fail("a map holds a key twice");
        }
        fresh.reindex();
        m = std::move(fresh);
        return true;
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<M>, [](SchemaBuilder& bb) -> schema::TypeDescriptor {
            auto k = durable<K>::describe(bb);
            auto v = durable<V>::describe(bb);
            return schema::Map{k, v};
        });
    }
    using mut = MapMut<K, V>;
};

}  // namespace kladde

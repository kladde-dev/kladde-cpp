// The core of the typed layer: how values encode, the backend that records
// their mutations, the regions that say where a value lies, the `durable`
// trait every durable type specializes, and the schema builder.
#pragma once

#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <kladde/error.hpp>
#include <kladde/schema.hpp>
#include <kladde/store.hpp>
#include <kladde/varint.hpp>

namespace kladde {

using bytes_view = std::span<std::uint8_t const>;

// ---------------------------------------------------------------------------
// Encoding and decoding

/// Where values encode to: bytes appended at the end.
using Encoder = std::vector<std::uint8_t>;

/// Appends `n` bytes of `v`, least significant first.
template <class U>
void put_le(Encoder& out, U v, std::size_t n = sizeof(U)) {
    auto u = static_cast<std::uint64_t>(v);
    for (std::size_t i = 0; i < n; ++i) out.push_back(static_cast<std::uint8_t>(u >> (8 * i)));
}

/// Reads values from the front of a byte sequence, remembering the first
/// failure.
class Decoder {
public:
    explicit Decoder(bytes_view in) : in_(in), size_(in.size()) {}

    bool ok() const { return error_.empty(); }
    bool at_end() const { return in_.empty(); }
    std::size_t position() const { return size_ - in_.size(); }
    std::string const& message() const { return error_; }

    /// Records a failure, the first one only; returns false so that a
    /// decode can `return d.fail(...)`.
    bool fail(std::string message) {
        if (error_.empty())
            error_ = std::move(message) + " at byte " + std::to_string(position());
        return false;
    }
    std::optional<std::uint64_t> varint() {
        auto v = get_varint(in_);
        if (!v) fail("a malformed varint");
        return v;
    }
    std::optional<std::uint8_t> byte() {
        if (in_.empty()) {
            fail("the encoding ends early");
            return std::nullopt;
        }
        auto b = in_[0];
        in_ = in_.subspan(1);
        return b;
    }
    std::optional<std::uint64_t> le(std::size_t n) {
        if (in_.size() < n) {
            fail("the encoding ends early");
            return std::nullopt;
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v |= std::uint64_t(in_[i]) << (8 * i);
        in_ = in_.subspan(n);
        return v;
    }
    std::optional<bytes_view> bytes(std::uint64_t n) {
        if (in_.size() < n) {
            fail("the encoding ends early");
            return std::nullopt;
        }
        auto b = in_.subspan(0, static_cast<std::size_t>(n));
        in_ = in_.subspan(static_cast<std::size_t>(n));
        return b;
    }
    /// The bytes up to the next `0xFF`, and skips it.
    std::optional<bytes_view> until_terminator() {
        for (std::size_t i = 0; i < in_.size(); ++i) {
            if (in_[i] == 0xFF) {
                auto b = in_.subspan(0, i);
                in_ = in_.subspan(i + 1);
                return b;
            }
        }
        fail("text without its terminator");
        return std::nullopt;
    }

private:
    bytes_view in_;
    std::size_t size_;
    std::string error_;
};

// ---------------------------------------------------------------------------
// The backend

/// What the typed layer records mutations through: a `Store`, or for tests a
/// plain byte vector. Positions are absolute positions in the sequence.
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::uint64_t size() const = 0;
    virtual expected<std::optional<CutId>> write(std::uint64_t pos, bytes_view bytes) = 0;
    virtual expected<std::optional<CutId>> splice(std::uint64_t pos, std::uint64_t removed,
                                                  bytes_view bytes) = 0;
    virtual expected<void> move(std::uint64_t from, std::uint64_t len, std::uint64_t to) = 0;
    virtual expected<void> paste(std::uint64_t pos, std::uint64_t removed, CutId cut,
                                 bytes_view bytes) = 0;
    virtual bool is_alive(CutId const& cut) const = 0;
    virtual expected<void> begin_transaction() = 0;
    virtual expected<void> end_transaction() = 0;
    virtual void abandon_transaction() = 0;
    virtual expected<void> begin_batch() = 0;
    virtual expected<void> end_batch() = 0;
    virtual void poison() = 0;
    virtual bool is_poisoned() const = 0;

    /// Runs `f`, which returns an `expected`, as one transaction, unless a
    /// transaction is open already, in which case `f` joins it. A failure
    /// of `f`, or an exception leaving it, abandons the transaction, which
    /// poisons the backend.
    template <class F>
    auto atomically(F&& f) -> decltype(f()) {
        bool outer = depth_ == 0;
        if (outer) {
            if (auto r = begin_transaction(); !r) return std::move(r).error();
        }
        ++depth_;
#if KLADDE_EXCEPTIONS
        try {
#endif
            auto r = f();
            --depth_;
            if (outer) {
                if (!r) {
                    abandon_transaction();
                    return r;
                }
                if (auto e = end_transaction(); !e) return std::move(e).error();
            }
            return r;
#if KLADDE_EXCEPTIONS
        } catch (...) {
            --depth_;
            if (outer) abandon_transaction();
            poison();
            throw;
        }
#endif
    }

    /// Whether a transaction is open.
    bool in_transaction() const { return depth_ > 0; }

protected:
    int depth_ = 0;
    friend class TransactionScope;
};

/// A `Backend` over a `Store`.
class StoreBackend final : public Backend {
public:
    explicit StoreBackend(Store& store) : store_(&store) {}
    std::uint64_t size() const override { return store_->size(); }
    expected<std::optional<CutId>> write(std::uint64_t pos, bytes_view bytes) override {
        return store_->write(pos, bytes);
    }
    expected<std::optional<CutId>> splice(std::uint64_t pos, std::uint64_t removed,
                                          bytes_view bytes) override {
        return store_->splice(pos, removed, bytes);
    }
    expected<void> move(std::uint64_t from, std::uint64_t len, std::uint64_t to) override {
        return store_->move(from, len, to);
    }
    expected<void> paste(std::uint64_t pos, std::uint64_t removed, CutId cut,
                         bytes_view bytes) override {
        return store_->paste(pos, removed, cut, bytes);
    }
    bool is_alive(CutId const& cut) const override { return store_->is_alive(cut); }
    expected<void> begin_transaction() override { return store_->begin_transaction(); }
    expected<void> end_transaction() override { return store_->end_transaction(); }
    void abandon_transaction() override { store_->abandon_transaction(); }
    expected<void> begin_batch() override { return store_->begin_batch(); }
    expected<void> end_batch() override { return store_->end_batch(); }
    void poison() override { store_->poison(); }
    bool is_poisoned() const override { return store_->is_poisoned(); }

private:
    Store* store_;
};

/// A `Backend` over a byte vector, which applies every mutation at once: the
/// reference a store is tested against, and a way to test values without a
/// store. Every cut is alive, and a paste writes the cut's bytes.
class VectorBackend final : public Backend {
public:
    explicit VectorBackend(std::vector<std::uint8_t> bytes = {}) : bytes_(std::move(bytes)) {}
    std::vector<std::uint8_t> const& bytes() const { return bytes_; }

    std::uint64_t size() const override { return bytes_.size(); }
    expected<std::optional<CutId>> write(std::uint64_t pos, bytes_view b) override {
        if (pos + b.size() > bytes_.size()) return errc::out_of_bounds;
        std::copy(b.begin(), b.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(pos));
        return cut(b.size());
    }
    expected<std::optional<CutId>> splice(std::uint64_t pos, std::uint64_t removed,
                                          bytes_view b) override {
        if (pos + removed > bytes_.size()) return errc::out_of_bounds;
        auto at = bytes_.begin() + static_cast<std::ptrdiff_t>(pos);
        at = bytes_.erase(at, at + static_cast<std::ptrdiff_t>(removed));
        bytes_.insert(at, b.begin(), b.end());
        return cut(removed);
    }
    expected<void> move(std::uint64_t from, std::uint64_t len, std::uint64_t to) override {
        if (from + len > bytes_.size() || to + len > bytes_.size()) return errc::out_of_bounds;
        std::vector<std::uint8_t> moved(bytes_.begin() + static_cast<std::ptrdiff_t>(from),
                                        bytes_.begin() + static_cast<std::ptrdiff_t>(from + len));
        bytes_.erase(bytes_.begin() + static_cast<std::ptrdiff_t>(from),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(from + len));
        bytes_.insert(bytes_.begin() + static_cast<std::ptrdiff_t>(to), moved.begin(), moved.end());
        return {};
    }
    expected<void> paste(std::uint64_t pos, std::uint64_t removed, CutId,
                         bytes_view b) override {
        auto r = splice(pos, removed, b);
        if (!r) return std::move(r).error();
        return {};
    }
    bool is_alive(CutId const&) const override { return true; }
    expected<void> begin_transaction() override { return {}; }
    expected<void> end_transaction() override { return {}; }
    void abandon_transaction() override { poisoned_ = true; }
    expected<void> begin_batch() override { return {}; }
    expected<void> end_batch() override { return {}; }
    void poison() override { poisoned_ = true; }
    bool is_poisoned() const override { return poisoned_; }

private:
    std::optional<CutId> cut(std::uint64_t len) {
        if (len == 0) return std::nullopt;
        return CutId{next_++, len};
    }
    std::vector<std::uint8_t> bytes_;
    std::uint64_t next_ = 0;
    bool poisoned_ = false;
};

// ---------------------------------------------------------------------------
// Regions: where a value lies

/// A value that keeps where its children start: a struct's field sizes, or
/// a container's index of its elements' sizes.
class Node {
public:
    virtual std::uint64_t offset_of(std::size_t index) const = 0;
    virtual void resized(std::size_t index, std::uint64_t old_size, std::uint64_t new_size) = 0;

protected:
    ~Node() = default;
};

/// A value that knows its own start in the sequence at any time, however
/// the values around it change: the root of a file, or an element of a
/// `kladde::List`. Long-lived handles are rooted at anchors.
class Anchor {
public:
    virtual std::uint64_t start() const = 0;
    /// The anchored value's encoding changed size by `delta`.
    virtual void grew(std::int64_t delta) = 0;
    virtual Backend& backend() const = 0;
    /// Finds the value at address `target` inside the anchored value, adding
    /// its offset within it to `offset`.
    virtual bool locate(void const* target, std::uint64_t& offset) const = 0;

protected:
    ~Anchor() = default;
};

/// The sizes of a struct's fields, for handles made together, each of which
/// finds where it starts whenever it writes.
class FieldSizes final : public Node {
public:
    void fill(std::uint64_t base, std::span<std::optional<std::size_t> const> fixed,
              std::span<std::uint64_t const> sizes) {
        base_ = base;
        sizes_.assign(sizes.begin(), sizes.end());
        constant_ = fixed.size();
        for (std::size_t i = 0; i < fixed.size(); ++i) {
            if (!fixed[i]) {
                constant_ = i + 1;
                break;
            }
        }
    }
    /// Whether field `index` starts at a constant offset: no field before
    /// it changes size.
    bool is_constant(std::size_t index) const { return index < constant_; }
    std::uint64_t size(std::size_t index) const { return sizes_[index]; }
    std::uint64_t offset_of(std::size_t index) const override {
        std::uint64_t at = base_;
        for (std::size_t i = 0; i < index; ++i) at += sizes_[i];
        return at;
    }
    void resized(std::size_t index, std::uint64_t, std::uint64_t new_size) override {
        sizes_[index] = new_size;
    }

private:
    std::uint64_t base_ = 0;
    std::vector<std::uint64_t> sizes_;
    std::size_t constant_ = 0;
};

/// Where a region starts and whom it tells when it changes size; the
/// links of nested regions form a chain up to an anchor.
class Link {
public:
    enum class Kind { anchored, offset, indexed };

    static Link at_anchor(Anchor* anchor, std::uint64_t len) {
        Link l;
        l.kind_ = Kind::anchored;
        l.anchor_ = anchor;
        l.len_ = len;
        return l;
    }

    std::uint64_t len() const { return len_; }

    std::uint64_t start() const {
        switch (kind_) {
            case Kind::anchored: return anchor_->start();
            case Kind::offset: return base_->start() + offset_;
            case Kind::indexed: return owner_->start() + node_->offset_of(index_);
        }
        return 0;
    }

    void resized(std::uint64_t old_size, std::uint64_t new_size) {
        if (old_size == new_size) return;
        len_ = new_size;
        if (kind_ == Kind::anchored) {
            anchor_->grew(static_cast<std::int64_t>(new_size) - static_cast<std::int64_t>(old_size));
            return;
        }
        if (node_) node_->resized(index_, old_size, new_size);
        auto before = owner_->len_;
        owner_->resized(before, before - old_size + new_size);
    }

    /// The link of a child at `at` within this one, `len` long, whose size
    /// changes `node` hears about, if any, at `index`.
    Link child(std::uint64_t at, std::uint64_t len, Node* node, std::size_t index) {
        Link l;
        l.kind_ = Kind::offset;
        if (kind_ == Kind::offset) {
            l.base_ = base_;
            l.offset_ = offset_ + at;
        } else {
            l.base_ = this;
            l.offset_ = at;
        }
        l.len_ = len;
        l.owner_ = this;
        l.node_ = node;
        l.index_ = index;
        return l;
    }

    /// The link of a child that finds where it starts from `node` whenever
    /// it writes.
    Link together(std::uint64_t len, Node* node, std::size_t index) {
        Link l;
        l.kind_ = Kind::indexed;
        l.len_ = len;
        l.owner_ = this;
        l.node_ = node;
        l.index_ = index;
        return l;
    }

private:
    Kind kind_ = Kind::anchored;
    std::uint64_t len_ = 0;
    Anchor* anchor_ = nullptr;
    Link* base_ = nullptr;  // for `offset`: the link the offset is from
    std::uint64_t offset_ = 0;
    Link* owner_ = nullptr;  // the parent, for `offset` and `indexed`
    Node* node_ = nullptr;
    std::size_t index_ = 0;
};

/// The substring of the sequence holding one value: writes and splices at
/// positions relative to its start, checked against its length.
///
/// A region made from another (`child`, `together`, `field`) refers to it,
/// so the parent must stay where it is while the child lives: a `Mut` must
/// not be moved while a `Mut` made from it is alive.
class Region {
public:
    Region(Backend& backend, Link link, Anchor* anchor)
        : backend_(&backend), link_(link), anchor_(anchor) {}

    Backend& backend() const { return *backend_; }
    /// The anchor of the nearest enclosing value that has one: where values
    /// placed in this region attach their lists.
    Anchor* anchor() const { return anchor_; }
    std::uint64_t size() const { return link_.len(); }
    std::uint64_t start() const { return link_.start(); }

    expected<std::optional<CutId>> write(std::uint64_t at, bytes_view bytes) const {
        if (!fits(at, bytes.size())) return errc::out_of_bounds;
        return backend_->write(start() + at, bytes);
    }
    expected<std::optional<CutId>> splice(std::uint64_t at, std::uint64_t removed,
                                          bytes_view bytes) {
        if (!fits(at, removed)) return errc::out_of_bounds;
        auto cut = backend_->splice(start() + at, removed, bytes);
        if (!cut) return cut;
        auto len = size();
        link_.resized(len, len - removed + bytes.size());
        return cut;
    }
    expected<void> move(std::uint64_t from, std::uint64_t len, std::uint64_t to) const {
        if (!fits(from, len) || !fits(to, len)) return errc::out_of_bounds;
        auto s = start();
        return backend_->move(s + from, len, s + to);
    }
    expected<void> paste(std::uint64_t at, std::uint64_t removed, CutId cut, bytes_view bytes) {
        if (!fits(at, removed)) return errc::out_of_bounds;
        if (auto r = backend_->paste(start() + at, removed, cut, bytes); !r) return r;
        auto len = size();
        link_.resized(len, len - removed + cut.length);
        return {};
    }
    template <class F>
    auto transaction(F&& f) -> decltype(f()) {
        return backend_->atomically(std::forward<F>(f));
    }

    Region child(std::uint64_t at, std::uint64_t len, Node* node, std::size_t index,
                 Anchor* anchor = nullptr) {
        assert(at + len <= size() && "a child inside its parent");
        return Region(*backend_, link_.child(at, len, node, index), anchor ? anchor : anchor_);
    }
    Region together(std::uint64_t len, Node* node, std::size_t index) {
        return Region(*backend_, link_.together(len, node, index), anchor_);
    }
    Region field(FieldSizes& fields, std::size_t index) {
        if (fields.is_constant(index))
            return child(fields.offset_of(index), fields.size(index), &fields, index);
        return together(fields.size(index), &fields, index);
    }

private:
    bool fits(std::uint64_t at, std::uint64_t len) const { return at + len >= at && at + len <= size(); }

    Backend* backend_;
    Link link_;
    Anchor* anchor_;
};

/// Reports `e` as a failure that ends the session: poisons `backend`, then
/// throws or calls the handler (see `kladde::report`).
inline void fail(Backend& backend, kladde::error e) {
    if (e.code != errc::out_of_bounds) backend.poison();
    report(std::move(e));
}

/// Checks `r`; on failure reports it as `fail` does and returns false.
template <class T>
bool check(Backend& backend, expected<T> const& r) {
    if (r) return true;
    fail(backend, r.error());
    return false;
}

// ---------------------------------------------------------------------------
// The schema builder

/// Builds a type table: each described type once, a reference to it
/// wherever it occurs, and recursive types through a slot reserved before
/// their descriptor is built.
class SchemaBuilder {
public:
    /// The reference of the type with `key`, describing it with `build` the
    /// first time.
    template <class F>
    schema::TypeRef describe(void const* key, F&& build) {
        if (auto it = by_key_.find(key); it != by_key_.end()) return it->second;
        schema::TypeRef r{slots_.size()};
        slots_.emplace_back();
        by_key_.emplace(key, r);
        auto d = build(*this);
        slots_[r.index] = std::move(d);
        return r;
    }
    /// A descriptor of its own, shared with no other type: an enum
    /// variant's struct of fields.
    template <class F>
    schema::TypeRef describe_anonymous(F&& build) {
        schema::TypeRef r{slots_.size()};
        slots_.emplace_back();
        auto d = build(*this);
        slots_[r.index] = std::move(d);
        return r;
    }
    schema::TypeTable finish() && {
        std::vector<schema::TypeDescriptor> ds;
        ds.reserve(slots_.size());
        for (auto& s : slots_) ds.push_back(std::move(*s));
        return schema::TypeTable(std::move(ds));
    }

private:
    std::vector<std::optional<schema::TypeDescriptor>> slots_;
    std::unordered_map<void const*, schema::TypeRef> by_key_;
};

/// A unique address per type, which the schema builder keys types by.
template <class T>
inline constexpr char type_key = 0;

// ---------------------------------------------------------------------------
// The trait

/// How a type is durable: specialized for every durable type.
///
/// A specialization provides
///
/// - `static constexpr std::optional<std::size_t> fixed_size`: the size every
///   value encodes to, if all encode to one;
/// - `static std::size_t encoded_len(T const&)`, `static void encode(T const&,
///   Encoder&)`, `static bool decode(Decoder&, T&)`;
/// - `static schema::TypeRef describe(SchemaBuilder&)`;
/// - `using mut = ...`, the handle type `Mut<T>` names;
///
/// and optionally `contains_list`, `locate` and `attach` (for types that hold a
/// `kladde::List`), and `fixed_form_size`, `encode_fixed`, `decode_fixed`
/// (for types with a fixed form).
template <class T, class Enable = void>
struct durable;

/// A type with a `durable` specialization.
template <class T>
concept Durable = requires(T const& v, Encoder& out, Decoder& in, T& t, SchemaBuilder& b) {
    { durable<T>::fixed_size } -> std::convertible_to<std::optional<std::size_t>>;
    { durable<T>::encoded_len(v) } -> std::convertible_to<std::size_t>;
    durable<T>::encode(v, out);
    { durable<T>::decode(in, t) } -> std::same_as<bool>;
    { durable<T>::describe(b) } -> std::same_as<schema::TypeRef>;
};

/// A durable type with a fixed form: an encoding of one size for every
/// value, which `kladde::Fixed<T>` stores.
template <class T>
concept Bounded = Durable<T> && requires(T const& v, Encoder& out, Decoder& in, T& t) {
    { durable<T>::fixed_form_size } -> std::convertible_to<std::size_t>;
    durable<T>::encode_fixed(v, out);
    { durable<T>::decode_fixed(in, t) } -> std::same_as<bool>;
};

/// The handle that mutates a durable value of type `T`: it records every
/// change in the file's journal first, and applies it to the value second.
///
/// A `Mut` is valid until something is written through a handle that is not
/// itself or made from it, and must not be moved while a handle made from it
/// is alive, as an iterator is invalidated by edits of its container.
template <class T>
using Mut = typename durable<std::remove_cvref_t<T>>::mut;

template <class T>
inline constexpr bool contains_list_v = [] {
    if constexpr (requires { durable<T>::contains_list; })
        return durable<T>::contains_list;
    else
        return false;
}();

/// Finds the value at address `target` inside `v`, adding its offset within
/// `v` to `offset`; for anchors to find the lists they hold.
template <class T>
bool locate(T const& v, void const* target, std::uint64_t& offset) {
    if constexpr (contains_list_v<T>)
        return durable<T>::locate(v, target, offset);
    else
        return false;
}

/// Tells every `kladde::List` inside `v`, outside other lists, that `anchor`
/// holds it.
template <class T>
void attach(T& v, Anchor* anchor) {
    if constexpr (contains_list_v<T>) durable<T>::attach(v, anchor);
}

template <Durable T>
std::size_t encoded_len(T const& v) {
    return durable<T>::encoded_len(v);
}

template <Durable T>
void encode(T const& v, Encoder& out) {
    durable<T>::encode(v, out);
}

/// The encoding of `v`.
///
/// ```cpp
/// assert(kladde::to_bytes(std::uint32_t{300}) == std::vector<std::uint8_t>{0xac, 0x02});
/// ```
template <Durable T>
std::vector<std::uint8_t> to_bytes(T const& v) {
    Encoder out;
    out.reserve(durable<T>::encoded_len(v));
    durable<T>::encode(v, out);
    return out;
}

/// Decodes a whole encoding of a `T`; fails with `errc::corrupt` if `bytes`
/// is not exactly one.
template <Durable T>
expected<T> from_bytes(bytes_view bytes) requires std::default_initializable<T> {
    Decoder in(bytes);
    T v{};
    if (!durable<T>::decode(in, v) || !in.ok()) return error{errc::corrupt, in.message()};
    if (!in.at_end()) return error{errc::corrupt, "bytes after the value"};
    return v;
}

template <Durable T>
schema::TypeRef describe(SchemaBuilder& b) {
    return durable<T>::describe(b);
}

/// `T`'s schema: its descriptor and every descriptor it reaches, rooted at
/// index 0.
template <Durable T>
schema::TypeTable schema_of() {
    SchemaBuilder b;
    durable<T>::describe(b);
    return std::move(b).finish();
}

/// `T`'s fingerprint, which a file records for its root type.
///
/// ```cpp
/// struct Point { std::int32_t x, y; };
/// KLADDE_STRUCT(Point, x, y)
/// assert(kladde::fingerprint_of<Point>().to_hex() == "...");
/// ```
template <Durable T>
schema::Fingerprint fingerprint_of() {
    return schema_of<T>().fingerprint();
}

template <class T>
inline constexpr std::optional<std::size_t> fixed_size_v = durable<T>::fixed_size;

/// The sum of `sizes`, if every one is known.
constexpr std::optional<std::size_t> sum_sizes(std::initializer_list<std::optional<std::size_t>> sizes) {
    std::size_t n = 0;
    for (auto s : sizes) {
        if (!s) return std::nullopt;
        n += *s;
    }
    return n;
}

/// The smallest of 1, 2, 4 and 8 bytes that holds `largest`.
constexpr std::uint8_t discriminant_width(std::uint64_t largest) {
    return largest <= 0xff ? 1 : largest <= 0xffff ? 2 : largest <= 0xffffffffu ? 4 : 8;
}

// ---------------------------------------------------------------------------
// Handles shared by many types

/// The base of every handle: the value it mutates and where it lies.
template <class T>
class MutBase {
public:
    MutBase(T& value, Region region) : value_(&value), region_(std::move(region)) {}
    MutBase(MutBase const&) = delete;
    MutBase& operator=(MutBase const&) = delete;
    MutBase(MutBase&&) = default;
    MutBase& operator=(MutBase&&) = default;

    /// The value, for reading.
    T const& get() const { return *value_; }
    T const& operator*() const { return *value_; }
    T const* operator->() const { return value_; }
    operator T const&() const { return *value_; }

    /// Replaces the whole value: one splice of its new encoding over the old.
    void set(T value) {
        auto bytes = to_bytes(value);
        if (!check(region_.backend(), region_.splice(0, region_.size(), bytes))) return;
        *value_ = std::move(value);
        attach(*value_, region_.anchor());
    }

    Region& region() { return region_; }

protected:
    T* value_;
    Region region_;
};

/// The handle of a value that changes only as a whole: a scalar, or a value
/// in its fixed form.
template <class T>
class SetMut : public MutBase<T> {
public:
    using MutBase<T>::MutBase;
    SetMut& operator=(T value) {
        this->set(std::move(value));
        return *this;
    }
};

/// Records the replacement of `old`, the bytes at `at` in `region`, by
/// `fresh` as one splice of the range between their longest common prefix
/// and suffix, or as nothing if they are equal.
inline expected<void> splice_changed(Region& region, std::uint64_t at, bytes_view old,
                                     bytes_view fresh) {
    std::size_t prefix = 0;
    while (prefix < old.size() && prefix < fresh.size() && old[prefix] == fresh[prefix]) ++prefix;
    std::size_t room = std::min(old.size(), fresh.size()) - prefix;
    std::size_t suffix = 0;
    while (suffix < room && old[old.size() - 1 - suffix] == fresh[fresh.size() - 1 - suffix])
        ++suffix;
    auto removed = old.size() - prefix - suffix;
    auto inserted = fresh.subspan(prefix, fresh.size() - prefix - suffix);
    if (removed == 0 && inserted.empty()) return {};
    auto r = region.splice(at + prefix, removed, inserted);
    if (!r) return std::move(r).error();
    return {};
}

/// A value cut out of a container, to be pasted into another without being
/// encoded again: the value, and the store's cut of its bytes.
///
/// While the journal that removed the value lasts, a paste writes none of
/// its bytes; after a flush has folded that journal, a paste encodes the
/// value again. Dropping a cut drops the value.
template <class T>
class Cut {
public:
    Cut(T value, std::optional<CutId> id, Backend& backend)
        : value_(std::move(value)), id_(id), backend_(&backend) {}
    Cut(Cut&&) = default;
    Cut& operator=(Cut&&) = default;

    T const& operator*() const { return value_; }
    T const* operator->() const { return &value_; }
    /// Whether a paste now would write none of the value's bytes.
    bool is_alive() const { return id_ && backend_->is_alive(*id_); }
    /// The value, given up without pasting it.
    T into_inner() && { return std::move(value_); }

    /// For containers: the cut's id, the value's encoding, and the value.
    std::optional<CutId> id() const { return id_; }
    T take() && { return std::move(value_); }

private:
    T value_;
    std::optional<CutId> id_;
    Backend* backend_;
};

}  // namespace kladde

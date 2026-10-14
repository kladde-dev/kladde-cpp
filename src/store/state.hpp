// The committed state of a store in memory: the span table, the rope over the
// sequence's spans, the short chains of the schema and the private state, the
// page table with its victim buckets, and the reverse lists from a page to
// the spans it holds.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "consts.hpp"
#include "treap.hpp"

namespace kladde::store {

/// A map keyed by a span id or a page number.
template <class V>
using IdMap = std::unordered_map<std::uint32_t, V>;
/// A set of span ids or page numbers.
using IdSet = std::unordered_set<std::uint32_t>;

/// Which chain a span belongs to.
enum class Chain : std::uint8_t {
    /// The span is free.
    none,
    sequence,
    schema,
    private_,
};

/// What a span shows.
struct Content {
    enum class Kind : std::uint8_t {
        /// The span id is free.
        free,
        /// Bytes in data page `page`, at byte offset `offset` within it.
        ref,
        /// The payload of its entry, at byte offset `offset` within the table
        /// page `page` that holds the entry.
        inline_,
        /// Zeros.
        zero,
        /// Bytes the flush in progress holds and has not placed yet: `page`
        /// is an index into its pending table.
        pending,
    };
    Kind kind = Kind::free;
    std::uint32_t page = 0;
    std::uint16_t offset = 0;

    static Content ref(std::uint32_t page, std::uint16_t offset) { return {Kind::ref, page, offset}; }
    static Content inline_at(std::uint32_t page, std::uint16_t offset) {
        return {Kind::inline_, page, offset};
    }
    static Content zero() { return {Kind::zero, 0, 0}; }
    static Content pending(std::uint32_t index) { return {Kind::pending, index, 0}; }

    bool operator==(Content const&) const = default;
};

/// A span's record in the span table.
struct Span {
    std::uint32_t len = 0;
    /// The next span of its chain, or 0.
    std::uint32_t next = 0;
    Content content;
    Chain chain = Chain::none;
    /// The table page holding its live entry, or `NONE` while it has none.
    std::uint32_t entry_page = 0;
    /// The bytes its live entry takes in that page, payload included.
    std::uint16_t entry_len = 0;
    /// Its place in that page's entry list.
    std::uint32_t tab = 0;
    /// Its place in its data page's span list, for a `Ref`.
    std::uint32_t rev = 0;
    /// Its node in the rope, for a span of the sequence.
    std::uint32_t node = 0;
    /// The flush that last wrote its bytes for the application, which ranks
    /// it for the header.
    std::uint64_t heat = 0;
};

/// What the rope holds: a span, or during a flush a run of bytes the flush
/// holds that has no span yet (a literal: `len` bytes of the flush's scratch
/// from `start`).
struct RopeItem {
    bool lit = false;
    /// The span's id, or the literal's start in the scratch.
    std::uint32_t id_or_start = 0;
    std::uint32_t len = 0;

    static RopeItem span(std::uint32_t id, std::uint32_t len) { return {false, id, len}; }
    static RopeItem literal(std::uint32_t start, std::uint32_t len) { return {true, start, len}; }

    std::uint64_t length() const { return len; }
    bool operator==(RopeItem const&) const = default;
};

/// Hands out span ids lowest first, from a bitmap of ids in use with a
/// summary bit per word that is full.
class Ids {
public:
    Ids();
    bool is_used(std::uint32_t id) const;
    /// Marks `id` in use.
    void mark(std::uint32_t id);
    /// Frees `id`.
    void free(std::uint32_t id);
    /// The lowest free id, marked in use; or `after + 1` if that is free, so
    /// that spans made one after another get consecutive ids.
    std::optional<std::uint32_t> alloc(std::uint32_t after);
    /// Ids in use.
    std::uint32_t count() const { return count_; }

private:
    std::vector<std::uint64_t> used_;
    std::vector<std::uint64_t> full_;
    std::uint32_t count_ = 0;
};

enum class PageKind : std::uint8_t {
    data,
    /// A leaf of the span table: a table page the header names.
    table,
    /// The governing header, or the slot the flush in progress writes.
    header,
};

/// Where a page sits in the victim buckets of its kind.
struct BucketSlot {
    std::uint8_t bucket = 0;
    std::uint32_t index = 0;
    bool operator==(BucketSlot const&) const = default;
};

/// The state of every page of the file.
///
/// Only a filling or live page has a kind and coverage, and only a live page
/// an epoch and a place among the victims.
struct Page {
    enum class State : std::uint8_t {
        /// In neither header's world: the flush may write it.
        reusable,
        /// In the world of the older header only; reusable after the next
        /// commit.
        fallback,
        /// One of the current journal's pages, or named by the header for it.
        journal,
        /// Taken by the flush in progress, which writes it at its commit.
        filling,
        /// In the governing header's world.
        live,
    };
    State state = State::reusable;
    /// The epoch the page was written in, for a live page.
    std::uint64_t epoch = 0;
    /// The bytes current state relies on: for a data page those its spans
    /// show, for a leaf its live entries' bytes.
    std::uint32_t coverage = 0;
    PageKind kind = PageKind::data;
    std::optional<BucketSlot> slot;

    static Page reusable() { return {}; }
    static Page fallback() { return {State::fallback, 0, 0, PageKind::data, std::nullopt}; }
    static Page journal() { return {State::journal, 0, 0, PageKind::data, std::nullopt}; }
    /// A page of kind `kind` that the flush in progress takes to write.
    static Page taken(PageKind kind) { return {State::filling, 0, 0, kind, std::nullopt}; }
    static Page live(std::uint64_t epoch, std::uint32_t coverage, PageKind kind) {
        return {State::live, epoch, coverage, kind, std::nullopt};
    }

    bool is(State s) const { return state == s; }
    bool is_live() const { return state == State::live; }
    bool operator==(Page const&) const = default;
};

/// Buckets per page kind.
inline constexpr std::size_t BUCKETS = 16;

/// Pages bucketed by coverage, for O(1) victim selection.
struct Buckets {
    std::array<std::vector<std::uint32_t>, BUCKETS> lists;

    static std::uint8_t bucket_of(std::uint32_t coverage);
    /// The sparsest non-empty bucket.
    std::optional<std::size_t> lowest_non_empty() const;
};

/// The committed state.
struct State {
    /// Every span, by id; free slots have a free content.
    std::vector<Span> spans;
    Ids ids;
    /// The rope over the sequence's spans, and its root.
    Treap<RopeItem> rope;
    std::uint32_t root = NIL;
    /// The spans of the schema's chain and of the private chain, in order.
    std::vector<std::uint32_t> schema;
    std::vector<std::uint32_t> private_;
    /// Every page of the file, by page number.
    std::vector<Page> pages;
    Buckets data_buckets;
    Buckets table_buckets;
    /// Data page -> the spans whose bytes lie in it.
    IdMap<std::vector<std::uint32_t>> data_spans;
    /// Table page -> the spans whose live entry it holds.
    IdMap<std::vector<std::uint32_t>> table_spans;
    /// Pages whose coverage fell since the last commit.
    IdSet dropped_candidates;
    /// The epoch of the flush in progress, and of every page it fills; or
    /// between flushes, the governing header's.
    std::uint64_t flush_epoch = 0;

    // ---------------------------------------------------------------- pages

    /// Makes room for page records up to `page`, which the file now holds.
    void ensure_page(std::uint32_t page);
    /// The record of `page`, which must be live.
    Page const& live(std::uint32_t page) const;
    Page& live_mut(std::uint32_t page);
    std::uint32_t coverage(std::uint32_t page) const;
    /// The record of `page` if it may be a consolidation victim: a live data
    /// page or leaf with live content.
    Page const* victim(std::uint32_t page) const;
    /// Moves `page` to the bucket its coverage calls for, or out of buckets.
    void rebucket(std::uint32_t page);
    /// Takes `page` out of its bucket, before it stops being live.
    void unbucket(std::uint32_t page);
    void cover(std::uint32_t page, std::uint32_t n);
    void uncover(std::uint32_t page, std::uint32_t n);

    // ---------------------------------------------------------------- spans

    Span const& span(std::uint32_t id) const { return spans[id]; }
    Span& span_mut(std::uint32_t id) { return spans[id]; }
    /// A new span id, preferably `after + 1`, with a fresh record.
    std::optional<std::uint32_t> new_span(std::uint32_t after);
    /// The length of the sequence.
    std::uint64_t seq_len() const { return rope.sum(root); }
    /// The first span of the sequence, or 0.
    std::uint32_t seq_head() const;
    /// Adds span `id`, a `Ref`, to its data page's list.
    void note_data(std::uint32_t id);
    /// Takes span `id`, a `Ref`, out of its data page's list.
    void unnote_data(std::uint32_t id);
    /// Records that span `id`'s live entry now lies in `page`, taking `len`
    /// bytes.
    void note_entry(std::uint32_t id, std::uint32_t page, std::uint16_t len);
    /// Ends span `id`'s live entry, if it has one: it no longer counts toward
    /// its page.
    void drop_entry(std::uint32_t id);
    /// Ends span `id`: its entry dies, its bytes stop counting, and its id is
    /// free.
    void kill(std::uint32_t id);
    /// The spans of the sequence, in order.
    std::vector<std::uint32_t> seq_spans() const;
    /// The span a rope node holds.
    std::uint32_t span_at(std::uint32_t node) const;

    /// Checks every invariant between flushes, aborting at the first
    /// violation: the rope against the span table, the chains' links, the
    /// reverse lists, and every page's coverage.
    void check(std::uint32_t header_slot) const;

private:
    std::uint32_t& coverage_mut(std::uint32_t page);
    std::optional<std::uint8_t> wanted_bucket(std::uint32_t page) const;
    std::optional<std::uint8_t> current_bucket(std::uint32_t page) const;
    Buckets& buckets_of(PageKind kind);
};

}  // namespace kladde::store

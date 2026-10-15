// The store's inner state, which `Store` owns, and the pieces of a flush in
// progress. The flush (flush.cpp), consolidation (consolidate.cpp) and the
// write phase (store.cpp) are its member functions.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <kladde/store.hpp>

#include "entries.hpp"
#include "fold.hpp"
#include "journal.hpp"
#include "page.hpp"
#include "state.hpp"

namespace kladde::store {

/// The transaction and batch state machine of
/// impl/transactions-and-batches.md, with the cursors into `ops` that a state
/// does not imply: `ops[..committed]` is in the journal pages,
/// `ops[committed..ready]` is a batch's ready run, and `ops[ready..]` is the
/// open transaction. `immediate` implies `committed == ready == end`,
/// `in_batch` implies `ready == end`, and `in_transaction` implies
/// `ready == committed`; a state ignores the fields it does not carry.
struct Tx {
    enum class Kind : std::uint8_t { immediate, in_batch, in_transaction, in_tx_in_batch };
    Kind kind = Kind::immediate;
    std::uint32_t depth = 0;
    std::uint32_t batch = 0;
    Cursor committed;
    Cursor ready;

    static Tx immediate() { return {}; }
    static Tx in_batch(std::uint32_t batch, Cursor committed) {
        return {Kind::in_batch, 0, batch, committed, {}};
    }
    static Tx in_transaction(std::uint32_t depth, std::uint32_t batch, Cursor committed) {
        return {Kind::in_transaction, depth, batch, committed, {}};
    }
    static Tx in_tx_in_batch(std::uint32_t depth, std::uint32_t batch, Cursor committed,
                             Cursor ready) {
        return {Kind::in_tx_in_batch, depth, batch, committed, ready};
    }

    /// The same state with its committed cursor at `c`.
    Tx with_committed(Cursor c) const {
        Tx t = *this;
        if (kind != Kind::immediate) t.committed = c;
        return t;
    }
};

/// A data page the flush fills.
struct DataPage {
    std::uint32_t page = 0;
    std::unique_ptr<PageBuf> buf;
    std::size_t used = 0;

    std::size_t room() const { return MAX_PAGE_CONTENT - used; }
};

/// What a span the flush made holds until it is placed: its bytes in the
/// flush's scratch, and whether its entry will hold them inline.
struct PendingBytes {
    std::uint32_t start = 0;
    bool inline_ = false;
};

/// What one flush keeps while it works.
struct Flush {
    /// Bytes of new spans and of runs that have no span yet.
    std::vector<std::uint8_t> scratch;
    std::vector<PendingBytes> pending;
    /// Spans that get a new entry in this flush.
    IdSet stated;
    /// Those of them whose entry goes to the header; the rest go to leaves.
    IdSet header;
    /// Payload pages whose bytes new spans show, which the commit takes into
    /// the file as data pages without writing them.
    std::vector<std::uint32_t> adopted;
};

/// What the flush writes, gathered before the commit.
struct Output {
    std::vector<DataPage> data;
    /// Leaves: page and buffer.
    std::vector<std::pair<std::uint32_t, std::unique_ptr<PageBuf>>> tables;
    std::vector<std::uint8_t> header_content;
    /// Payload pages taken into the file as they lie.
    std::vector<std::uint32_t> adopted;
};

/// What consolidation carries from flush to flush.
struct ConsState {
    /// The per-flush budget in pages, as the controller moved it.
    double budget = 0;
    /// Where sampling starts within a bucket; rotates so that no prefix of a
    /// bucket is examined forever.
    std::size_t rot = 0;
    /// Victims this flush found unusable or took already, so that it does not
    /// pick them again.
    IdSet skip;
    /// Whether this flush runs in compaction mode.
    bool compaction = false;
    /// Where the search for the highest live page resumes.
    std::uint32_t tail = 0;
    /// Where the rotating walk resumes: a span of the sequence, or 0 for its
    /// start.
    std::uint32_t walk = 0;
};

/// What one budgeted data page would hold, or what table victims would
/// restate, and what either would reclaim.
struct Offer {
    std::vector<std::uint32_t> whole;
    std::optional<std::uint32_t> cut;
    bool tail = false;
    double reclaimed = 0;
    double written = 0;
    double fill = 0;
};

/// A commit's `fsync` and header write, on a thread of their own
/// (impl/transactions-and-batches.md#a-flush-off-the-callers-thread).
struct PendingCommit {
    std::thread worker;
    /// What the worker reports, written by it and read once it is joined.
    std::unique_ptr<expected<void>> result;
    /// The pages the commit before it freed, which its `fsync` makes
    /// reusable.
    std::vector<std::uint32_t> release;
};

/// What `Store::atomically` saved when it began, to roll back to.
struct AtomicMark {
    Cursor end;
    Tx tx;
    std::uint64_t len = 0;
};

}  // namespace kladde::store

namespace kladde {

struct Store::Inner {
    using Cursor = store::Cursor;
    using Flush = store::Flush;
    using Output = store::Output;
    using DataPage = store::DataPage;
    using Page = store::Page;
    using PageBuf = store::PageBuf;

    Inner(std::unique_ptr<Storage> storage, Options opts, store::JournalWriter journal);
    /// Waits for a commit on its own thread: the file must not be left with
    /// a header half written by a thread nobody waits for.
    ~Inner();
    Inner(Inner const&) = delete;
    Inner& operator=(Inner const&) = delete;

    std::unique_ptr<Storage> storage;
    Options opts;
    bool poisoned = false;
    // The committed file.
    std::uint64_t epoch = 0;
    store::HeaderFields header;
    std::uint32_t header_slot = 0;
    store::State state;
    /// Resident span-table pages: the leaves and the governing header.
    store::IdMap<std::unique_ptr<PageBuf>> table_pages;
    std::set<std::uint32_t> ready;
    std::vector<std::uint32_t> retiring;
    std::uint32_t file_pages = 0;
    /// The schema chain's and the private chain's content, as committed.
    std::vector<std::uint8_t> schema;
    std::vector<std::uint8_t> private_;
    /// New content for them, for the next flush to write.
    std::optional<std::vector<std::uint8_t>> schema_pending;
    std::optional<std::vector<std::uint8_t>> private_pending;
    // The write phase.
    store::JournalWriter journal;
    /// Every operation since the last flush, appended to the journal pages or
    /// not.
    store::Records ops;
    store::Tx tx;
    /// The sequence's length as every operation so far leaves it.
    std::uint64_t len = 0;
    /// The cuts of the journal being appended to, as written so far.
    store::JournalCuts journal_cuts;
    /// The cuts of the journal pasted so far, by the number of the record that
    /// made each, with the number of the paste's record.
    std::map<std::uint64_t, std::uint64_t> pasted;
    bool first_flush = true;
    /// The page where the next journal begins, which the seal of the journal
    /// being folded named; the flush's header names it too.
    std::optional<std::uint32_t> sealed_next;
    /// A flush's `fsync` and header write, running on a thread of their own,
    /// and the pages that become reusable once they are done.
    std::optional<store::PendingCommit> pending;
    /// Whether the flush running was asked for, and so waits for its commit,
    /// rather than triggered by a mutation.
    bool explicit_flush = false;
    /// Data pages read at open, which decoding the sequence reads from;
    /// dropped by `end_load`.
    store::IdMap<std::unique_ptr<PageBuf>> mirror;
    /// Data pages the flush in progress read.
    store::IdMap<std::unique_ptr<PageBuf>> cache;
    store::ConsState cons;
    Stats stats;
    /// The marks of the open `atomically` calls, innermost last.
    std::vector<store::AtomicMark> atomic_marks;

    // ------------------------------------------------------------ store.cpp

    expected<void> record(store::Record const& r);
    CutId new_cut(std::uint64_t n) const;
    void truncate_ops(Cursor to);
    expected<Cursor> close(Cursor committed, Cursor ready_at, bool in_batch);
    expected<Cursor> append(Cursor from, Cursor to);
    expected<void> place_payloads(Cursor from, Cursor to);
    expected<void> write_open_payload();
    expected<void> ensure_journal_page();
    expected<std::uint64_t> replayed_len() const;
    expected<void> load_mirror();
    expected<PageBuf const*> page_bytes(std::uint32_t page);
    expected<void> check_range(std::uint64_t pos, std::uint64_t n) const;
    expected<std::uint64_t> grown(std::uint64_t removed, std::uint64_t added) const;
    expected<std::uint32_t> take_page(Page page);

    // ------------------------------------------------------------ flush.cpp

    /// The committed cursor, and the end of the operations outside any open
    /// transaction: `ops[committed..ready]` is a batch's ready run, or empty.
    std::pair<Cursor, Cursor> closed() const;
    /// Appends a batch's ready run to the journal, and returns the committed
    /// cursor after.
    expected<Cursor> append_ready();
    expected<void> flush_now();
    expected<void> join_pending();
    expected<void> seal();
    expected<bool> flush_if_journal_exceeds(double fraction);
    expected<void> fold_journal(Cursor committed);
    expected<void> flush_inner(Cursor committed);
    void kill_in(Flush& f, std::uint32_t id);
    expected<std::uint32_t> span_to_scratch(Flush& f, std::uint32_t id);
    expected<std::vector<std::uint32_t>> new_spans(Flush& f, std::uint32_t start, std::uint32_t total,
                                                   store::Chain chain, std::uint64_t heat,
                                                   std::uint32_t after);
    expected<void> apply_pieces(std::vector<store::Piece> const& pieces, Flush& f);
    void literal(Flush& f, std::uint64_t start, std::uint64_t count, std::uint32_t& root,
                 std::vector<std::uint32_t>& touched);
    expected<std::uint32_t> payload_span(Flush& f, store::Placement pl, std::uint32_t after);
    expected<void> boundary(std::uint64_t x, Flush& f);
    void insert_at(std::uint64_t x, std::uint32_t m);
    expected<void> settle(Flush& f, std::vector<std::uint32_t> const& touched);
    expected<void> write_small_chains(Flush& f);
    store::Body body(Flush const& f, std::uint32_t id) const;
    std::size_t estimate_of(Flush const& f, std::uint32_t id) const;
    std::vector<std::uint32_t> leaves() const;
    void choose_header(Flush& f);
    bool leafward(Flush const& f, std::uint32_t id) const;
    expected<std::uint32_t> merge_group(Flush& f, std::vector<std::uint32_t> const& group);
    expected<void> merge_evicted(Flush& f);
    expected<void> walk(Flush& f);
    expected<void> merge_run(Flush& f, std::vector<std::uint32_t> const& run);
    expected<DataPage> new_data_page();
    expected<void> place(Flush& f, DataPage& dp, std::uint32_t id);
    expected<std::uint32_t> split_span(Flush& f, std::uint32_t id, std::uint32_t head);
    std::vector<std::uint32_t> chunks(Flush const& f) const;
    expected<void> pack(Flush& f, Output& out);
    std::vector<std::pair<std::vector<std::uint8_t>, std::vector<store::Placed>>> encode_leaves(
        Flush const& f, std::vector<std::uint32_t> const& ids) const;
    std::optional<std::pair<std::vector<std::uint8_t>, std::vector<store::Placed>>> encode_header(
        Flush const& f, std::vector<std::uint32_t> const& children,
        std::vector<std::uint32_t> const& ids) const;
    expected<void> cut(Flush& f, Output& out);
    void note_placed(Flush const& f, std::uint32_t page, std::size_t content_at,
                     std::vector<store::Placed> const& placed);
    expected<void> commit(std::uint64_t e, Output out);
    expected<void> truncate(bool force);

    // ------------------------------------------------------------ compact.cpp

    expected<void> close_consolidated();

    // ------------------------------------------------------------ consolidate.cpp

    using VictimFilter = std::function<bool(Inner const&, std::uint32_t)>;

    void begin_consolidation();
    std::optional<std::uint32_t> tail();
    std::optional<std::uint32_t> compaction_tail(bool data);
    std::optional<std::uint32_t> compaction_victim(bool data);
    double score(std::uint32_t p) const;
    std::vector<std::uint32_t> sample(bool data, std::size_t bucket);
    std::optional<std::uint32_t> pick_victim(bool data, std::size_t from, std::uint32_t max,
                                             VictimFilter const& ok);
    std::vector<std::uint32_t> survivors(std::uint32_t v) const;
    expected<bool> evacuate_into(Flush& f, std::uint32_t v, DataPage& dp);
    expected<std::vector<std::uint32_t>> evacuate_across(Flush& f, std::uint32_t v, DataPage& dp);
    expected<void> free_fill(Flush& f, DataPage& open);
    std::optional<store::Offer> data_offer(std::uint32_t carry, bool may_cut);
    std::optional<store::Offer> table_offer(double pages);
    void rewrite_table_victims(Flush& f, std::vector<std::uint32_t> const& victims);
    expected<void> budget_loop(Flush& f, Output& out);
    std::vector<std::uint32_t> cut_fillers(Flush const& f, std::size_t room);
    void after_commit();
    void fill_stats(Stats& s) const;
};

}  // namespace kladde

// The store: one byte sequence, durable, edited by positions and bytes.
// It knows nothing about types; the typed layer (kladde/kladde.hpp) records
// every mutation through it. The file format, the journal, the flush and
// consolidation are content/spec and content/impl, ported from kladde-rs's
// kladde-store.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#include <kladde/error.hpp>

namespace kladde {

/// The bytes a store reads and writes: a file, or memory for tests.
///
/// Every method reports failure as `errc::io`.
class Storage {
public:
    virtual ~Storage() = default;
    /// The current length in bytes.
    virtual expected<std::uint64_t> size() const = 0;
    /// Reads exactly `buf.size()` bytes at `offset`, within the length.
    virtual expected<void> read_at(std::span<std::uint8_t> buf, std::uint64_t offset) const = 0;
    /// Writes `buf` at `offset`, within the length.
    virtual expected<void> write_at(std::span<std::uint8_t const> buf, std::uint64_t offset) = 0;
    /// Grows or shrinks to `len` bytes; bytes a growth exposes read as zero.
    virtual expected<void> resize(std::uint64_t len) = 0;
    /// Makes everything written so far durable: `fsync`, or
    /// `fcntl(F_FULLFSYNC)` on macOS.
    virtual expected<void> sync() = 0;
    /// A second handle to the same bytes, which another thread may sync and
    /// write through while this one writes elsewhere; null if there is none,
    /// which keeps every flush on the caller's thread.
    virtual std::unique_ptr<Storage> try_clone() const { return nullptr; }
};

/// A `Storage` over a file on disk, locked against a second process.
class FileStorage final : public Storage {
public:
    /// Opens an existing file for reading and writing.
    static expected<std::unique_ptr<FileStorage>> open(std::filesystem::path const& path);
    /// Creates the file, replacing any file there.
    static expected<std::unique_ptr<FileStorage>> create(std::filesystem::path const& path);
    ~FileStorage() override;

    expected<std::uint64_t> size() const override;
    expected<void> read_at(std::span<std::uint8_t> buf, std::uint64_t offset) const override;
    expected<void> write_at(std::span<std::uint8_t const> buf, std::uint64_t offset) override;
    expected<void> resize(std::uint64_t len) override;
    expected<void> sync() override;
    /// A second handle to the same open file, sharing its lock.
    std::unique_ptr<Storage> try_clone() const override;

private:
    explicit FileStorage(int fd) : fd_(fd) {}
    int fd_;
};

/// A `Storage` in memory, for tests: shares its bytes with its copies, can
/// simulate a power cut by keeping any subset of the writes since the last
/// sync, and can be told to fail.
class MemoryStorage final : public Storage {
public:
    MemoryStorage();
    /// A storage that records the writes since the last sync, so that
    /// `crash_image` can drop some of them.
    static MemoryStorage with_crash_tracking();
    /// A storage holding `image`, a file's bytes.
    static MemoryStorage from_image(std::vector<std::uint8_t> image);
    /// A storage holding `image`, all of it durable, that records the writes
    /// from now on as `with_crash_tracking` does.
    static MemoryStorage from_image_with_crash_tracking(std::vector<std::uint8_t> image);

    /// The bytes as they stand.
    std::vector<std::uint8_t> image() const;
    /// The number of writes since the last sync.
    std::size_t unsynced_writes() const;
    /// The bytes after a power cut that kept write `i` since the last sync
    /// exactly when `keep(i)` is true.
    std::vector<std::uint8_t> crash_image(std::function<bool(std::size_t)> const& keep) const;
    /// Makes every later sync fail.
    void fail_syncs();
    /// Makes every write, resize and sync fail after `n` more succeed.
    void fail_after(std::uint64_t n);

    expected<std::uint64_t> size() const override;
    expected<void> read_at(std::span<std::uint8_t> buf, std::uint64_t offset) const override;
    expected<void> write_at(std::span<std::uint8_t const> buf, std::uint64_t offset) override;
    expected<void> resize(std::uint64_t len) override;
    expected<void> sync() override;
    /// A copy, sharing the bytes; it may be used from another thread.
    std::unique_ptr<Storage> try_clone() const override;

    struct State;

private:
    std::shared_ptr<State> state_;
};

/// The size of a page of a kladde file, in bytes.
inline constexpr std::size_t page_size = 4096;
/// The most bytes a data page holds.
inline constexpr std::size_t max_page_content = page_size - 15;

/// Tuning knobs. Everything here is policy: two stores with different
/// options write files that read the same. The names and defaults are
/// kladde-rs's `Options`.
struct Options {
    /// The journal pages the journal is kept within: a transaction that would
    /// not fit in what is left of them is preceded by a flush, and a batch is
    /// cut where its next operation would not fit. Empty, the default, sizes
    /// the budget to the sequence, as `journal_budget` describes; a value
    /// fixes it at that many pages.
    std::optional<std::uint32_t> journal_budget_pages;
    /// Whether a flush seals the journal it folds, naming in it where the
    /// next journal begins, so that recovery reaches the next journal from
    /// either header.
    bool seal = true;
    /// Whether a flush that a mutation triggers hands its `fsync` and its
    /// header write to a thread of its own, so that the mutation returns
    /// without waiting for the disk while later mutations append to the next
    /// journal. It does so only after sealing, and only with a storage whose
    /// `try_clone` gives a second handle; an explicit flush, `close`, the next
    /// flush and destroying the store wait for it, and a failure on that
    /// thread poisons the store when they do.
    bool sync_on_worker = false;
    std::uint32_t inline_threshold = 64;
    std::uint32_t rewrite_below = 64;
    std::uint32_t span_min = 64;
    std::uint32_t cold_flushes = 16;
    std::size_t walk = 256;
    double mu = 0.02;
    /// Whether flushes consolidate (reclaim space by moving live content).
    bool consolidate = true;
    std::uint32_t budget_pages = 8;
    std::uint32_t budget_min = 1;
    std::uint32_t budget_max = 256;
    double target_fill = 0.8;
    double churn_floor = 1.0;
    double theta = 0.05;
    std::size_t lookahead = 16;
    std::size_t sample = 8;
    double hole_share = 0.25;
    std::uint32_t truncate_tail = 16;

    /// The share of the sequence's length that the journal budget is sized
    /// to when `journal_budget_pages` is empty.
    static constexpr double journal_budget_share = 0.02;
    /// The fewest journal pages a budget sized to the sequence allows.
    static constexpr std::uint32_t journal_budget_min = 2;
    /// The most journal pages a budget sized to the sequence allows.
    static constexpr std::uint32_t journal_budget_max = 10;

    /// The journal budget, in pages, for a sequence of `len` bytes: the
    /// fixed `journal_budget_pages` if set, and otherwise
    /// `journal_budget_share` of `len` in whole pages, within
    /// `journal_budget_min` and `journal_budget_max`. The store sizes the
    /// budget to the sequence as it stands whenever it checks the journal
    /// against it.
    ///
    /// ```cpp
    /// kladde::Options automatic;
    /// assert(automatic.journal_budget(50'000) == 2);
    /// assert(automatic.journal_budget(1'000'000) == 4);
    /// assert(automatic.journal_budget(100'000'000) == 10);
    /// kladde::Options fixed;
    /// fixed.journal_budget_pages = 16;
    /// assert(fixed.journal_budget(50'000) == 16);
    /// ```
    std::uint32_t journal_budget(std::uint64_t len) const {
        if (journal_budget_pages) return *journal_budget_pages;
        auto pages = static_cast<std::uint32_t>(double(len) * journal_budget_share / double(page_size));
        return std::clamp(pages, journal_budget_min, journal_budget_max);
    }
};

/// Counters describing the file and the work done on it since it was opened.
///
/// ```cpp
/// auto store = kladde::Store::create(std::make_unique<kladde::MemoryStorage>()).value();
/// store.splice(0, 0, std::vector<std::uint8_t>(20000, 1)).value();
/// store.flush().value();
/// assert(store.stats().data_pages == 5 && store.stats().live_fraction() > 0.9);
/// ```
struct Stats {
    /// Flushes committed.
    std::uint64_t flushes = 0;
    /// Transactions appended to the journal.
    std::uint64_t transactions = 0;
    /// Journal stream bytes appended.
    std::uint64_t journal_bytes = 0;
    /// Data pages written by flushes.
    std::uint64_t data_pages_written = 0;
    /// Span-table pages written by flushes, headers excluded.
    std::uint64_t table_pages_written = 0;
    /// Header pages written.
    std::uint64_t headers_written = 0;
    /// Pages read after opening.
    std::uint64_t pages_read = 0;
    /// Times the file grew by a page.
    std::uint64_t file_grew = 0;
    /// Times the file shrank.
    std::uint64_t truncations = 0;
    /// Data pages consolidation emptied by moving their live bytes.
    std::uint64_t evacuated_pages = 0;
    /// Live bytes consolidation moved.
    std::uint64_t evacuated_bytes = 0;
    /// Victims taken in pages the flush wrote anyway.
    std::uint64_t free_filled_pages = 0;
    /// Data pages opened only to take victims.
    std::uint64_t budget_pages = 0;
    /// Flushes that ran in compaction mode, moving content off the end of the
    /// file.
    std::uint64_t compaction_flushes = 0;
    /// Leaves whose live entries were moved, to empty them.
    std::uint64_t table_rewrites = 0;
    /// Bytes of new content flushes wrote into data pages.
    std::uint64_t fresh_bytes = 0;
    /// Entries written, in the header and in leaves.
    std::uint64_t entries_stated = 0;
    /// Spans split where an edit cut into them.
    std::uint64_t span_splits = 0;
    /// Bytes of short spans written again where an edit cut into them, rather
    /// than split.
    std::uint64_t rewritten_bytes = 0;
    /// Merges of short spans as their entries left the header, and the bytes
    /// they wrote again.
    std::uint64_t evict_merges = 0;
    std::uint64_t evict_merge_bytes = 0;
    /// Merges of cold spans by the rotating walk, and the bytes they wrote
    /// again.
    std::uint64_t walk_merges = 0;
    std::uint64_t walk_merge_bytes = 0;
    /// Flushes whose `fsync` and header write ran on a thread of their own
    /// (`Options::sync_on_worker`).
    std::uint64_t commits_on_worker = 0;

    // A snapshot of the file, taken when the stats are read.
    /// Pages in the file.
    std::uint64_t file_pages = 0;
    /// Pages the journal takes.
    std::uint64_t journal_pages = 0;
    /// Live data pages.
    std::uint64_t data_pages = 0;
    /// Live span-table pages, the headers excluded.
    std::uint64_t table_pages = 0;
    /// Pages within the file that hold nothing live: reusable, or retired by
    /// the last commit.
    std::uint64_t free_pages = 0;
    /// Bytes of data pages the current state relies on.
    std::uint64_t live_data_bytes = 0;
    /// Bytes of leaves the current state relies on.
    std::uint64_t live_table_bytes = 0;
    /// The sequence's length.
    std::uint64_t sequence_bytes = 0;
    /// The schema chain's length.
    std::uint64_t schema_bytes = 0;
    /// Spans the chains reach.
    std::uint64_t spans = 0;
    /// The consolidation budget in pages.
    std::uint64_t budget = 0;

    /// Live bytes over the content capacity of every live data page and leaf;
    /// 1 for a file without such pages.
    double live_fraction() const {
        std::uint64_t pages = data_pages + table_pages;
        if (pages == 0) return 1.0;
        return double(live_data_bytes + live_table_bytes) / (double(pages) * double(max_page_content));
    }
};

/// The bytes a write or splice removed, which a paste may name instead of
/// writing them again while the journal that removed them lasts.
struct CutId {
    std::uint64_t record = 0;
    std::uint64_t length = 0;
};

/// One byte sequence, durable.
///
/// Every mutation is in the journal when it returns: `write`, `splice`,
/// `move` and `paste`, at positions in the sequence. Flushes fold the
/// journal into the file, by themselves as it fills, and on `flush`.
/// A store is used from one thread at a time. A mutation or flush that fails
/// to write the file poisons it, and every later call fails with
/// `errc::poisoned`; one refused for its arguments (`errc::out_of_bounds`,
/// or `errc::corrupt` for a misused cut or scope) records nothing and leaves
/// the store usable.
///
/// ```cpp
/// auto store = kladde::Store::create(std::make_unique<kladde::MemoryStorage>()).value();
/// store.splice(0, 0, bytes("hello")).value();
/// store.flush().value();
/// assert(store.read_all().value() == bytes("hello"));
/// ```
class Store {
public:
    /// Creates an empty store in `storage`, discarding what it held.
    static expected<Store> create(std::unique_ptr<Storage> storage, Options opts = {});
    /// Opens the store in `storage`, recovering every transaction that the
    /// journal holds completely, and folding a non-empty recovered journal.
    static expected<Store> open(std::unique_ptr<Storage> storage, Options opts = {});

    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    ~Store();

    /// The sequence's length.
    std::uint64_t size() const;

    /// Overwrites `bytes.size()` bytes at `pos`; returns the cut of the bytes
    /// it overwrote, if any.
    expected<std::optional<CutId>> write(std::uint64_t pos, std::span<std::uint8_t const> bytes);
    /// Replaces `removed` bytes at `pos` with `bytes`; returns the cut of the
    /// removed bytes, if any.
    expected<std::optional<CutId>> splice(std::uint64_t pos, std::uint64_t removed,
                                          std::span<std::uint8_t const> bytes);
    /// Moves `len` bytes at `from` to `to`, a position in the sequence
    /// without them.
    expected<void> move(std::uint64_t from, std::uint64_t len, std::uint64_t to);
    /// Replaces `removed` bytes at `pos` with the bytes of `cut`, which are
    /// `bytes`: a Paste record while the journal that made the cut lasts,
    /// and an ordinary splice of `bytes` after.
    expected<void> paste(std::uint64_t pos, std::uint64_t removed, CutId cut,
                         std::span<std::uint8_t const> bytes);
    /// Whether a paste of `cut` now would write none of its bytes.
    bool is_alive(CutId const& cut) const;

    /// Opens a transaction: the mutations until `end_transaction` reach the
    /// journal together, and a crash keeps all of them or none.
    expected<void> begin_transaction();
    expected<void> end_transaction();
    /// Discards the open transaction's mutations and poisons the store: the
    /// in-memory values already hold them, and the file will not.
    void abandon_transaction();
    /// Opens a batch: mutations are appended in as few pieces as the journal's
    /// budget allows.
    expected<void> begin_batch();
    expected<void> end_batch();

    /// Runs `f`, which returns an `expected`, as one transaction: what it
    /// records reaches the journal together once it succeeds, and a crash
    /// keeps all of it or none. If `f` fails, what it recorded is discarded
    /// and its error returned, as if it had never run, and the store stays
    /// usable; if the append at the end fails, the store is poisoned. Nests
    /// inside other transactions and batches.
    ///
    /// ```cpp
    /// auto r = store.atomically([&]() -> kladde::expected<void> {
    ///     KLADDE_TRY(store.splice(0, 1, bytes("xyz")));
    ///     KLADDE_TRY(store.write(100, bytes("past the end")));  // fails
    ///     return {};
    /// });
    /// assert(!r && store.size() == 3);  // of "abc": the splice is gone too
    /// ```
    template <class F>
    auto atomically(F&& f) -> std::invoke_result_t<F&> {
        using R = std::invoke_result_t<F&>;
        if (auto b = begin_atomically(); !b) return R(std::move(b).error());
        R r = f();
        if (r) {
            if (auto e = end_atomically(); !e) return R(std::move(e).error());
        } else {
            roll_back_atomically();
        }
        return r;
    }

    /// Folds the journal into the file now.
    expected<void> flush();
    /// Flushes if the journal holds at least `fraction` of its budget,
    /// counting what the flush would append; returns whether it flushed.
    expected<bool> flush_if_journal_exceeds(double fraction);
    /// Flushes, then shrinks the file to end at its last needed page.
    expected<void> close();

    /// The schema chain's content.
    std::vector<std::uint8_t> schema() const;
    void set_schema(std::vector<std::uint8_t> bytes);
    /// The root fingerprint the header records.
    std::array<std::uint8_t, 16> root_fingerprint() const;
    void set_root_fingerprint(std::array<std::uint8_t, 16> fingerprint);
    /// The private chain's content, as `schema` gives the schema's: the
    /// implementation's own state, which nothing else reads.
    std::vector<std::uint8_t> private_state() const;
    /// Sets the private chain's content, which the next flush writes.
    void set_private_state(std::vector<std::uint8_t> bytes);

    /// Calls `chunk` with the sequence's bytes in order, as of the last flush,
    /// in pieces; stops early, returning false, if `chunk` returns false.
    /// The pieces point into the store's pages: `chunk` must not call the
    /// store, and must copy what it keeps.
    expected<void> read(std::function<bool(std::span<std::uint8_t const>)> const& chunk) const;
    /// The whole sequence, as of the last flush.
    expected<std::vector<std::uint8_t>> read_all() const;
    /// Tells the store that the application has decoded the sequence, so
    /// that it may drop what it cached for that.
    void end_load();

    Stats stats() const;
    bool is_poisoned() const;
    void poison();
    /// Checks the store's internal invariants, aborting if one fails: for
    /// tests.
    void check() const;
    /// A description of the sequence's spans, in order, for debugging.
    std::string describe() const;

    struct Inner;

private:
    explicit Store(std::unique_ptr<Inner> inner);
    expected<void> begin_atomically();
    expected<void> end_atomically();
    void roll_back_atomically();
    std::unique_ptr<Inner> inner_;
};

}  // namespace kladde

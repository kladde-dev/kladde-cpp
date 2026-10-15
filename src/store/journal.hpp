// The journal: records, their encoding, the buffer that holds them between
// flushes, transactions, the chain of journal pages, the CRC chain, and
// recovery.
//
// The records act on positions of the one sequence: `Write`, `Splice`,
// `Move`, and `Paste`, which inserts what a `Write` or `Splice` earlier in
// the same journal cut out. On file each position is written as its distance
// from where the record before it left off, and the record's kind rides in
// the low two bits of that varint.
//
// In memory, a `Paste` names its cut by the index of the record that made it,
// and holds its value's encoding as well: if the record that made the cut
// belongs to a journal already folded when the paste is appended, the cut has
// ended, and the paste is written as a `Splice` of those bytes instead. The
// encoder and the fold apply the same rule, so they agree on every record; on
// file, a `Paste` names its cut by how many cuts of the journal were made
// after it.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

#include <kladde/store.hpp>

#include "crc.hpp"
#include "page.hpp"

namespace kladde::store {

/// A byte range of a `Records` arena, at a position counted from the start
/// of the session.
struct Bytes {
    std::uint64_t start = 0;
    std::uint32_t len = 0;
    bool operator==(Bytes const&) const = default;
};

/// One record: a byte-level effect on the sequence.
struct Record {
    enum class Kind : std::uint8_t {
        /// Overwrites `[pos, pos + bytes.len)`; what it overwrites is a cut
        /// of its journal.
        write = 0,
        /// Replaces the `removed` bytes at `pos` with `bytes`; what it
        /// removes is a cut of its journal, if anything.
        splice = 1,
        /// Cuts `[pos, pos + len)` and inserts it at `to`, a position in the
        /// sequence without the cut bytes.
        move = 2,
        /// Replaces the `removed` bytes at `pos` with the cut that the record
        /// numbered `cut` made; the cut is `len` bytes long. `bytes`, if
        /// `has_fallback`, holds those bytes' encoding, which is what the
        /// paste writes if that cut has ended by the time the paste is
        /// appended; a paste recovered from the journal has none, since its
        /// cut is in the journal with it.
        paste = 3,
    };

    Kind kind = Kind::write;
    /// The position, or for a `Move`, where the moved run starts.
    std::uint64_t pos = 0;
    std::uint64_t removed = 0;
    /// The length of a `Move`'s run or of a `Paste`'s cut.
    std::uint64_t len = 0;
    std::uint64_t to = 0;
    std::uint64_t cut = 0;
    Bytes bytes;
    bool has_fallback = false;

    static Record write(std::uint64_t pos, Bytes bytes) {
        return {Kind::write, pos, 0, 0, 0, 0, bytes, false};
    }
    static Record splice(std::uint64_t pos, std::uint64_t removed, Bytes bytes) {
        return {Kind::splice, pos, removed, 0, 0, 0, bytes, false};
    }
    static Record move(std::uint64_t from, std::uint64_t len, std::uint64_t to) {
        return {Kind::move, from, 0, len, to, 0, {}, false};
    }
    static Record paste(std::uint64_t pos, std::uint64_t removed, std::uint64_t cut,
                        std::uint64_t len, std::optional<Bytes> fallback) {
        return {Kind::paste, pos, removed, len, 0, cut, fallback.value_or(Bytes{}),
                fallback.has_value()};
    }

    bool operator==(Record const&) const = default;

    /// The record as written to a journal whose first record is numbered
    /// `first`: a `Paste` whose cut a record before `first` made has ended,
    /// and becomes a `Write` or `Splice` of its fallback bytes.
    Record as_written(std::uint64_t first) const;

    /// The length of the cut this record makes, as written: what a `Write`
    /// overwrites, or what a `Splice` removes, if anything.
    std::optional<std::uint64_t> cut_len() const;

    /// Where the record leaves off: the cursor after it.
    std::uint64_t end() const;
};

/// A position in a `Records` buffer, counted from the start of the session:
/// the records before it, the arena bytes they stash, and the bytes they
/// encode to. Discarding a prefix of the buffer moves its front and leaves
/// every cursor into the rest valid.
struct Cursor {
    /// Records before the position.
    std::uint64_t at = 0;
    /// Arena bytes before the position.
    std::uint64_t arena = 0;
    /// Encoded record bytes before the position, as their lengths were
    /// estimated when they were pushed.
    std::uint64_t bytes = 0;
    /// Where the records before the position left off in the sequence, which
    /// the next record's length is estimated from.
    std::uint64_t seq = 0;
    bool operator==(Cursor const&) const = default;
};

/// The cuts of the journal being appended to: which records are its own, and
/// which of them made a cut, in order, so that a `Paste` can be written with
/// how many cuts were made after its own.
struct JournalCuts {
    /// The number of the journal's first record: a cut that a record before
    /// it made has ended.
    std::uint64_t first = 0;
    /// The numbers of the records appended to the journal that made a cut,
    /// ascending.
    std::vector<std::uint64_t> made;

    static JournalCuts starting_at(std::uint64_t first) { return {first, {}}; }

    /// How many of the journal's cuts were made after the one record `cut`
    /// made.
    std::uint32_t distance(std::uint64_t cut) const;
};

/// Where a run of a record's bytes lies in a payload page: `len` bytes at
/// byte `offset` of page `page`, written in epoch `epoch`, whose CRC-32C is
/// `crc` (spec/journal.md#external-bytes).
struct Placement {
    std::uint32_t len = 0;
    std::uint32_t page = 0;
    std::uint16_t offset = 0;
    std::uint32_t crc = 0;
    std::uint64_t epoch = 0;

    /// The run's address in the file.
    std::uint64_t address() const { return std::uint64_t{page} * PAGE_SIZE + offset; }
    bool operator==(Placement const&) const = default;
};

/// The one buffer of every record since the last flush, with their payloads
/// in an arena beside them, and where each of the journal's transactions
/// starts among them.
///
/// Records are appended at the end and discarded from the front, and
/// positions count from the start of the session: cursors, and the `Bytes`
/// of a record, stay valid however much of the front goes.
class Records {
public:
    /// The position of the first record held.
    Cursor front() const { return front_; }
    /// The position past the last record held.
    Cursor end() const { return end_; }

    /// Stores `bytes` in the arena, as the payload of the record pushed next.
    Bytes stash(std::span<std::uint8_t const> bytes);
    /// Appends `r`, whose payload, if any, `stash` stored.
    void push(Record const& r);
    /// Marks the record at `at` as the first of a journal transaction.
    void mark_start(std::uint64_t at);

    /// Calls `f(starts, at, record)` for the records in `[from, to)`, with
    /// whether a journal transaction starts at each and its number in the
    /// session; stops at the first error `f` returns.
    template <class F>
    expected<void> for_range(Cursor from, Cursor to, F&& f) const {
        for (std::uint64_t at = from.at; at < to.at; ++at) {
            KLADDE_TRY(f(is_start(at), at, records_[at - front_.at]));
        }
        return {};
    }

    /// Whether a journal transaction starts at the record numbered `at`.
    bool is_start(std::uint64_t at) const;

    /// The record numbered `at` in the session, if the buffer still holds it.
    Record const* get(std::uint64_t at) const;
    /// Every record held.
    std::deque<Record> const& all() const { return records_; }

    /// The arena's bytes `[start, start + len)`.
    std::span<std::uint8_t const> slice(std::uint64_t start, std::size_t len) const;
    /// Copies the arena's bytes from position `start` into `out`.
    void copy_bytes(std::uint64_t start, std::span<std::uint8_t> out) const;
    /// Appends the bytes of `b` to `out`.
    void extend_bytes(Bytes b, std::vector<std::uint8_t>& out) const;
    /// The bytes of `b`, copied.
    std::vector<std::uint8_t> to_vec(Bytes b) const;

    /// Encodes the records in `[from, to)` as a transaction's record bytes,
    /// their positions relative to `cursor`, which ends where they leave off,
    /// for the journal whose cuts so far `cuts` holds.
    void encode(Cursor from, Cursor to, std::uint64_t& cursor, JournalCuts& cuts,
                std::vector<std::uint8_t>& out) const;

    /// Discards the records from `to` on: the end moves back to `to`.
    void truncate(Cursor to);
    /// Discards the records before `to`: the front moves up to `to`.
    void discard_to(Cursor to);

    /// Records that the arena's bytes from `start` on lie in a payload page
    /// as `p` says.
    void place(std::uint64_t start, Placement p) { placed_[start] = p; }
    /// The placed runs that overlap the arena's bytes `[start, start + len)`,
    /// in order, each with the arena position it starts at.
    std::vector<std::pair<std::uint64_t, Placement>> placed_in(std::uint64_t start,
                                                               std::uint64_t len) const;

private:
    /// Encodes `r`, as written, after a record that left off at `cursor`: a
    /// `Write` or `Splice` whose bytes are partly or wholly placed becomes
    /// one record per run, external where the run is placed, the first
    /// removing what `r` removes and the rest removing nothing, so that it
    /// still makes one cut.
    void encode_written(Record const& r, std::uint64_t cursor, std::uint32_t distance,
                        std::vector<std::uint8_t>& out) const;

    std::deque<Record> records_;
    /// Runs of the arena that also lie in a payload page, by the arena
    /// position they start at. A run never reaches past the bytes of the
    /// record it belongs to.
    std::map<std::uint64_t, Placement> placed_;
    /// The arena, its first `arena_skip_` bytes already discarded.
    std::vector<std::uint8_t> arena_;
    std::size_t arena_skip_ = 0;
    /// The record indices at which the journal's transactions start, for the
    /// records appended to it; ascending.
    std::deque<std::uint64_t> starts_;
    Cursor front_;
    Cursor end_;

    std::size_t arena_len() const { return arena_.size() - arena_skip_; }
};

/// The bytes the encoding of `r` takes after a record that left off at
/// `cursor`, if a `Paste` among them names its cut `distance` cuts back,
/// computed without encoding it.
std::size_t record_len(Record const& r, std::uint64_t cursor, std::uint32_t distance);

/// The stream bytes a transaction of `n` record bytes takes: its length
/// prefix, its records, and its `crc`.
std::uint64_t framed_len(std::uint64_t n);

/// The cuts of a journal as recovery decodes it: for each, the number of the
/// record that made it, its length, and whether it was pasted, oldest first.
struct DecodedCut {
    std::uint64_t record = 0;
    std::uint64_t len = 0;
    bool pasted = false;
};
using DecodedCuts = std::vector<DecodedCut>;

/// The bytes of an external reference as recovery reads them: its address,
/// length and checksum in, the bytes and where they lie out, or nothing if
/// the page does not hold them (spec/journal.md#recovery).
using Fetch = std::function<expected<std::optional<std::pair<std::vector<std::uint8_t>, Placement>>>(
    std::uint64_t address, std::uint32_t len, std::uint32_t crc)>;

/// Decodes the records of one transaction in `input` into `into`, their
/// positions relative to `cursor`, which ends where they leave off, adding
/// the cuts they make to the journal's `cuts`; checks the bounds and the cuts
/// a `Paste` names, but not whether the positions lie inside the sequence,
/// which only the fold knows. External bytes come from `fetch`.
///
/// Returns `false`, having taken nothing of the transaction into `into`,
/// `cursor` or `cuts`, if `fetch` finds an external reference's bytes
/// missing: the transaction belongs to the torn tail.
expected<bool> decode_records(std::span<std::uint8_t const> input, std::uint64_t& cursor,
                              DecodedCuts& cuts, Records& into, Fetch const& fetch);

/// The records of a seal's transaction: the escape (a `Write` of no bytes at
/// delta 0), escape 0, and the page where the next epoch's journal begins
/// (spec/journal.md#seals).
std::vector<std::uint8_t> seal_payload(std::uint32_t page);

/// The page a transaction's records name if they are a seal.
std::optional<std::uint32_t> parse_seal(std::span<std::uint8_t const> payload);

/// Supplies a page, existing in the file, for the journal to continue on.
using NextPage = std::function<expected<std::uint32_t>(Storage&)>;

/// Appends transactions to the journal's chain of pages.
class JournalWriter {
public:
    /// An empty journal of epoch `epoch`, starting at `first_page`.
    JournalWriter(std::uint64_t epoch, std::uint32_t first_page);
    /// The non-empty journal that recovery found: `transactions` complete
    /// transactions in `pages`, naming the payload pages `payload`.
    static JournalWriter recovered(std::uint64_t epoch, std::vector<std::uint32_t> pages,
                                   std::vector<std::uint32_t> payload, std::uint64_t transactions);

    /// Whether no transaction has been appended.
    bool is_empty() const { return transactions == 0; }
    /// Whether this is the journal of an earlier session, which nothing may
    /// be appended to.
    bool is_recovered() const { return recovered_; }
    /// Whether a transaction of about `n` record bytes, appended now, would
    /// keep the journal within `budget` pages. The next page is taken only
    /// once the stream continues past the current one, so a stream that ends
    /// within `budget` pages' worth of stream bytes takes at most `budget`
    /// pages.
    bool fits(std::uint64_t n, std::uint32_t budget) const;
    /// Whether the journal, with its payload pages, has outgrown `budget`
    /// pages, which only a transaction too large for them on its own makes it
    /// do.
    bool over_budget(std::uint32_t budget) const;
    /// What the journal holds against its budget: its stream bytes, and a
    /// page's worth of stream for each payload page.
    std::uint64_t held() const {
        return bytes + payload_pages.size() * std::uint64_t{JOURNAL_STREAM};
    }
    /// The page being appended to.
    std::uint32_t current_page() const { return pages.back(); }

    /// Appends one transaction whose record bytes are `payload`. `next_page`
    /// supplies a page, existing in the file, whenever the stream continues
    /// past the current one; the current page must exist too.
    expected<void> append(std::span<std::uint8_t const> payload, Storage& storage,
                          NextPage const& next_page);

    /// The journal pages in chain order; the first is the header's
    /// `journal_pointer`.
    std::vector<std::uint32_t> pages;
    /// Where the last record appended left off in the sequence, which the
    /// next record's positions are written relative to.
    std::uint64_t cursor = 0;
    /// Stream bytes appended so far.
    std::uint64_t bytes = 0;
    /// Transactions appended so far.
    std::uint64_t transactions = 0;
    /// The journal's epoch, which its payload pages' framing carries.
    std::uint64_t epoch = 0;
    /// The payload pages its transactions name, in the order they were taken.
    std::vector<std::uint32_t> payload_pages;
    /// The last payload page, while it has room: its number, its buffer, and
    /// the content bytes it holds.
    struct OpenPayload {
        std::uint32_t page = 0;
        std::unique_ptr<PageBuf> buf;
        std::size_t used = 0;
    };
    std::optional<OpenPayload> open;

private:
    expected<void> stream(std::span<std::uint8_t const> bytes, bool feed, Storage& storage,
                          NextPage const& next_page);
    expected<void> write_range(Storage& storage, std::size_t from, std::size_t to) const;

    std::unique_ptr<PageBuf> buf_;
    std::size_t pos_ = 0;
    std::size_t dirty_from_ = 0;
    Crc32c chain_;
    /// The journal of an earlier session, which nothing may be appended to:
    /// it names the pages that the flush folding it retires, and counts as
    /// full, so that the flush rules fold it before anything is appended.
    bool recovered_ = false;
};

/// What recovery found in the journal.
struct Recovered {
    /// The records of the complete transactions, in order, each transaction's
    /// start marked.
    Records records;
    /// How many complete transactions there were, seals included.
    std::size_t transactions = 0;
    /// The pages the valid part of the journals occupies.
    std::vector<std::uint32_t> pages;
    /// The payload pages its complete transactions name, in the order they
    /// were first named.
    std::vector<std::uint32_t> payload_pages;
};

/// Reads the valid prefix of the journal of epoch `epoch` starting at
/// `first_page`, and of the journals of later epochs that seals lead to.
expected<Recovered> recover(Storage const& storage, std::uint64_t epoch, std::uint32_t first_page);

}  // namespace kladde::store

#include "journal.hpp"

#include <algorithm>
#include <cstring>

#include "util.hpp"

namespace kladde::store {

namespace {

/// What a journal transaction may be off by from the estimate its records'
/// lengths give, since the cursor the estimate assumed may have been reset by
/// a flush in between: a head's delta, at most, grows by this much.
constexpr std::uint64_t ESTIMATE_SLACK = 16;

/// The position the head's delta counts to.
std::uint64_t head_pos(Record const& r) { return r.pos; }

/// The head of `r`, written after a record that left off at `cursor`.
std::uint64_t head(Record const& r, std::uint64_t cursor) {
    auto delta = static_cast<std::int64_t>(head_pos(r)) - static_cast<std::int64_t>(cursor);
    auto shifted = static_cast<std::int64_t>(static_cast<std::uint64_t>(delta) << 2);
    return zz(shifted | static_cast<std::int64_t>(r.kind));
}

std::size_t vlen(std::uint64_t v) { return varint_len(v); }

std::uint64_t move_delta(Record const& r) {
    return zz(static_cast<std::int64_t>(r.to) - static_cast<std::int64_t>(r.pos));
}

/// What the encoding of the record numbered `at` is estimated to take when it
/// is pushed: a `Paste` as a paste, its distance at most the number of records
/// since its cut's. The encoding itself can differ, if the paste's cut ends
/// before it is appended; the estimates only steer when to append and flush.
std::size_t estimated_len(Record const& r, std::uint64_t cursor, std::uint64_t at) {
    std::uint32_t distance = 0;
    if (r.kind == Record::Kind::paste) {
        std::uint64_t d = at > r.cut ? at - r.cut : 0;
        distance = d > UINT32_MAX ? UINT32_MAX : static_cast<std::uint32_t>(d);
    }
    return record_len(r, cursor, distance);
}

void encode_record(Record const& r, std::uint64_t cursor, std::uint32_t distance,
                   Records const& recs, std::vector<std::uint8_t>& out) {
    put_varint(out, head(r, cursor));
    switch (r.kind) {
        case Record::Kind::write:
            put_varint(out, r.bytes.len);
            recs.extend_bytes(r.bytes, out);
            break;
        case Record::Kind::splice:
            put_varint(out, r.removed);
            put_varint(out, r.bytes.len);
            recs.extend_bytes(r.bytes, out);
            break;
        case Record::Kind::move:
            put_varint(out, r.len);
            put_varint(out, move_delta(r));
            break;
        case Record::Kind::paste:
            put_varint(out, r.removed);
            put_varint(out, distance);
            break;
    }
}

/// The CRC chain of the journal, seeded with its epoch.
Crc32c seeded_chain(std::uint64_t epoch) {
    Crc32c c;
    c.update(le_bytes(epoch));
    return c;
}

}  // namespace

// ---------------------------------------------------------------- Record

Record Record::as_written(std::uint64_t first) const {
    if (kind == Kind::paste && has_fallback && cut < first) {
        KLADDE_DEBUG_CHECK(bytes.len == len, "a paste's fallback differs from its cut in length");
        if (removed == len) return Record::write(pos, bytes);
        return Record::splice(pos, removed, bytes);
    }
    return *this;
}

std::optional<std::uint64_t> Record::cut_len() const {
    switch (kind) {
        case Kind::write: return bytes.len;
        case Kind::splice: return removed > 0 ? std::optional<std::uint64_t>(removed) : std::nullopt;
        default: return std::nullopt;
    }
}

std::uint64_t Record::end() const {
    switch (kind) {
        case Kind::write:
        case Kind::splice: return pos + bytes.len;
        case Kind::move: return to + len;
        case Kind::paste: return pos + len;
    }
    return 0;
}

// ---------------------------------------------------------------- JournalCuts

std::uint32_t JournalCuts::distance(std::uint64_t cut) const {
    auto it = std::lower_bound(made.begin(), made.end(), cut);
    KLADDE_CHECK(it != made.end() && *it == cut, "a paste is appended to the journal of its cut");
    return static_cast<std::uint32_t>(made.end() - it - 1);
}

// ---------------------------------------------------------------- Records

Bytes Records::stash(std::span<std::uint8_t const> bytes) {
    std::uint64_t start = front_.arena + arena_len();
    arena_.insert(arena_.end(), bytes.begin(), bytes.end());
    return Bytes{start, static_cast<std::uint32_t>(bytes.size())};
}

void Records::push(Record const& r) {
    end_.bytes += estimated_len(r, end_.seq, end_.at);
    end_.at += 1;
    end_.seq = r.end();
    end_.arena = front_.arena + arena_len();
    records_.push_back(r);
}

void Records::mark_start(std::uint64_t at) {
    if (starts_.empty() || starts_.back() != at) {
        KLADDE_DEBUG_CHECK(starts_.empty() || starts_.back() < at, "transaction starts out of order");
        starts_.push_back(at);
    }
}

bool Records::is_start(std::uint64_t at) const {
    return std::binary_search(starts_.begin(), starts_.end(), at);
}

Record const* Records::get(std::uint64_t at) const {
    if (at < front_.at) return nullptr;
    std::uint64_t i = at - front_.at;
    if (i >= records_.size()) return nullptr;
    return &records_[i];
}

std::span<std::uint8_t const> Records::slice(std::uint64_t start, std::size_t len) const {
    std::size_t i = arena_skip_ + static_cast<std::size_t>(start - front_.arena);
    return std::span(arena_.data() + i, len);
}

void Records::copy_bytes(std::uint64_t start, std::span<std::uint8_t> out) const {
    auto s = slice(start, out.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
}

void Records::extend_bytes(Bytes b, std::vector<std::uint8_t>& out) const {
    auto s = slice(b.start, b.len);
    out.insert(out.end(), s.begin(), s.end());
}

std::vector<std::uint8_t> Records::to_vec(Bytes b) const {
    std::vector<std::uint8_t> out;
    extend_bytes(b, out);
    return out;
}

void Records::encode(Cursor from, Cursor to, std::uint64_t& cursor, JournalCuts& cuts,
                     std::vector<std::uint8_t>& out) const {
    for (std::uint64_t at = from.at; at < to.at; ++at) {
        Record w = records_[at - front_.at].as_written(cuts.first);
        std::uint32_t distance = w.kind == Record::Kind::paste ? cuts.distance(w.cut) : 0;
        encode_record(w, cursor, distance, *this, out);
        if (w.cut_len()) cuts.made.push_back(at);
        cursor = w.end();
    }
}

void Records::truncate(Cursor to) {
    KLADDE_DEBUG_CHECK(front_.at <= to.at && to.at <= end_.at, "truncating outside the buffer");
    records_.resize(to.at - front_.at);
    arena_.resize(arena_skip_ + (to.arena - front_.arena));
    while (!starts_.empty() && starts_.back() >= to.at) starts_.pop_back();
    end_ = to;
}

void Records::discard_to(Cursor to) {
    KLADDE_DEBUG_CHECK(front_.at <= to.at && to.at <= end_.at, "discarding outside the buffer");
    records_.erase(records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(to.at - front_.at));
    arena_skip_ += to.arena - front_.arena;
    // The discarded front is given back once it outweighs what is left, so
    // that discarding costs amortized constant time per byte.
    if (arena_skip_ > arena_len()) {
        arena_.erase(arena_.begin(), arena_.begin() + static_cast<std::ptrdiff_t>(arena_skip_));
        arena_skip_ = 0;
    }
    while (!starts_.empty() && starts_.front() < to.at) starts_.pop_front();
    front_ = to;
}

// ---------------------------------------------------------------- lengths

std::size_t record_len(Record const& r, std::uint64_t cursor, std::uint32_t distance) {
    std::size_t n = vlen(head(r, cursor));
    switch (r.kind) {
        case Record::Kind::write: return n + vlen(r.bytes.len) + r.bytes.len;
        case Record::Kind::splice:
            return n + vlen(r.removed) + vlen(r.bytes.len) + r.bytes.len;
        case Record::Kind::move: return n + vlen(r.len) + vlen(move_delta(r));
        case Record::Kind::paste: return n + vlen(r.removed) + vlen(distance);
    }
    return n;
}

std::uint64_t framed_len(std::uint64_t n) { return varint_len(n) + n + 4; }

// ---------------------------------------------------------------- decoding

expected<void> decode_records(std::span<std::uint8_t const> input, std::uint64_t& cursor,
                              DecodedCuts& cuts, Records& into) {
    std::span<std::uint8_t const> rest = input;
    auto get = [&]() -> expected<std::uint64_t> {
        auto v = get_varint(rest);
        if (!v) return corrupt("a journal record is truncated");
        return *v;
    };
    auto bounded = [](std::uint64_t v) -> expected<std::uint64_t> {
        if (v >= MAX_SEQUENCE) return corrupt("a journal record reaches past 2^48 bytes");
        return v;
    };
    while (!rest.empty()) {
        std::uint64_t at = into.end().at;
        KLADDE_TRY_ASSIGN(std::uint64_t h, get());
        std::int64_t x = unzigzag(h);
        std::int64_t kind = x & 3;
        std::int64_t p = static_cast<std::int64_t>(cursor) + (x >> 2);
        if (p < 0) return corrupt("a journal record names a negative position");
        KLADDE_TRY_ASSIGN(std::uint64_t pos, bounded(static_cast<std::uint64_t>(p)));
        Record rec;
        if (kind == 0 || kind == 1) {
            std::uint64_t removed = 0;
            if (kind == 1) {
                KLADDE_TRY_ASSIGN(std::uint64_t rm, get());
                KLADDE_TRY_ASSIGN(removed, bounded(rm));
            }
            KLADDE_TRY_ASSIGN(std::uint64_t len, get());
            if (rest.size() < len) return corrupt("a journal record's bytes are truncated");
            Bytes bytes = into.stash(rest.first(len));
            rest = rest.subspan(len);
            if (kind == 0) {
                if (len == 0) return corrupt("a journal record uses the reserved escape");
                cuts.push_back({at, len, false});
                rec = Record::write(pos, bytes);
            } else {
                if (removed > 0) cuts.push_back({at, removed, false});
                rec = Record::splice(pos, removed, bytes);
            }
        } else if (kind == 2) {
            KLADDE_TRY_ASSIGN(std::uint64_t l, get());
            KLADDE_TRY_ASSIGN(std::uint64_t len, bounded(l));
            KLADDE_TRY_ASSIGN(std::uint64_t d, get());
            std::int64_t to = 0;
            if (__builtin_add_overflow(static_cast<std::int64_t>(pos), unzigzag(d), &to) || to < 0)
                return corrupt("a move names a negative position");
            KLADDE_TRY_ASSIGN(std::uint64_t t, bounded(static_cast<std::uint64_t>(to)));
            rec = Record::move(pos, len, t);
        } else {
            KLADDE_TRY_ASSIGN(std::uint64_t rm, get());
            KLADDE_TRY_ASSIGN(std::uint64_t removed, bounded(rm));
            KLADDE_TRY_ASSIGN(std::uint64_t cut, get());
            if (cut >= cuts.size()) return corrupt("a paste names a cut its journal did not make");
            std::size_t i = cuts.size() - 1 - static_cast<std::size_t>(cut);
            if (cuts[i].pasted) return corrupt("a paste names a cut already pasted");
            cuts[i].pasted = true;
            rec = Record::paste(pos, removed, cuts[i].record, cuts[i].len, std::nullopt);
        }
        cursor = rec.end();
        into.push(rec);
    }
    return {};
}

// ---------------------------------------------------------------- JournalWriter

JournalWriter::JournalWriter(std::uint64_t epoch, std::uint32_t first_page)
    : pages{first_page}, buf_(new_page()), chain_(seeded_chain(epoch)) {}

JournalWriter JournalWriter::recovered(std::uint64_t epoch, std::vector<std::uint32_t> pages,
                                       std::uint64_t transactions) {
    JournalWriter w(epoch, 0);
    w.pages = std::move(pages);
    w.transactions = transactions;
    w.recovered_ = true;
    return w;
}

bool JournalWriter::fits(std::uint64_t n, std::uint32_t budget) const {
    return !recovered_ &&
           bytes + framed_len(n) + ESTIMATE_SLACK <= std::uint64_t{budget} * JOURNAL_STREAM;
}

bool JournalWriter::over_budget(std::uint32_t budget) const {
    return bytes > std::uint64_t{budget} * JOURNAL_STREAM;
}

expected<void> JournalWriter::append(std::span<std::uint8_t const> payload, Storage& storage,
                                     NextPage const& next_page) {
    KLADDE_DEBUG_CHECK(!recovered_, "appending to a recovered journal");
    if (payload.size() > UINT32_MAX) return errc::out_of_bounds;
    std::vector<std::uint8_t> len;
    put_varint(len, payload.size());
    KLADDE_TRY(stream(len, true, storage, next_page));
    KLADDE_TRY(stream(payload, true, storage, next_page));
    auto crc = le_bytes(chain_.value());
    KLADDE_TRY(stream(crc, false, storage, next_page));
    KLADDE_TRY(write_range(storage, dirty_from_, pos_));
    dirty_from_ = pos_;
    bytes += len.size() + payload.size() + 4;
    transactions += 1;
    return {};
}

expected<void> JournalWriter::stream(std::span<std::uint8_t const> in, bool feed, Storage& storage,
                                     NextPage const& next_page) {
    while (!in.empty()) {
        if (pos_ == JOURNAL_STREAM) {
            KLADDE_TRY_ASSIGN(std::uint32_t next, next_page(storage));
            auto next_bytes = le_bytes(next);
            std::memcpy(buf_->data() + JOURNAL_STREAM, next_bytes.data(), 4);
            chain_.update(next_bytes);
            store_u32(buf_->data() + JOURNAL_STREAM + 4, chain_.value());
            KLADDE_TRY(write_range(storage, dirty_from_, PAGE_SIZE));
            pages.push_back(next);
            pos_ = 0;
            dirty_from_ = 0;
        }
        std::size_t k = std::min(in.size(), JOURNAL_STREAM - pos_);
        std::memcpy(buf_->data() + pos_, in.data(), k);
        if (feed) chain_.update(in.first(k));
        pos_ += k;
        in = in.subspan(k);
    }
    return {};
}

expected<void> JournalWriter::write_range(Storage& storage, std::size_t from, std::size_t to) const {
    if (from < to) {
        std::uint64_t page = current_page();
        KLADDE_TRY(storage.write_at(std::span(buf_->data() + from, to - from),
                                    page * PAGE_SIZE + from));
    }
    return {};
}

// ---------------------------------------------------------------- recovery

namespace {

struct StreamReader {
    Storage const& storage;
    std::uint64_t file_pages;
    std::unique_ptr<PageBuf> buf = new_page();
    std::size_t pos = 0;
    Crc32c chain;
    std::vector<std::uint32_t> pages;

    /// Reads `out.size()` stream bytes, following trailers. `false` means the
    /// stream ended before that: a trailer failed its check or pointed
    /// nowhere valid.
    expected<bool> read(std::span<std::uint8_t> out, bool feed) {
        std::size_t done = 0;
        while (done < out.size()) {
            if (pos == JOURNAL_STREAM) {
                std::uint8_t const* next_bytes = buf->data() + JOURNAL_STREAM;
                std::uint32_t stored = load_u32(buf->data() + JOURNAL_STREAM + 4);
                Crc32c c = chain;
                c.update(std::span(next_bytes, 4));
                if (c.value() != stored) return false;
                std::uint32_t next = load_u32(next_bytes);
                if (next < 2 || next >= file_pages ||
                    std::find(pages.begin(), pages.end(), next) != pages.end())
                    return false;
                chain = c;
                KLADDE_TRY(storage.read_at(*buf, std::uint64_t{next} * PAGE_SIZE));
                pages.push_back(next);
                pos = 0;
            }
            std::size_t k = std::min(out.size() - done, JOURNAL_STREAM - pos);
            std::memcpy(out.data() + done, buf->data() + pos, k);
            if (feed) chain.update(out.subspan(done, k));
            pos += k;
            done += k;
        }
        return true;
    }
};

}  // namespace

expected<Recovered> recover(Storage const& storage, std::uint64_t epoch, std::uint32_t first_page) {
    KLADDE_TRY_ASSIGN(std::uint64_t file_len, storage.size());
    std::uint64_t file_pages = file_len / PAGE_SIZE;
    Recovered out;
    if (first_page < 2 || first_page >= file_pages) return out;
    StreamReader r{storage, file_pages, new_page(), 0, seeded_chain(epoch), {first_page}};
    KLADDE_TRY(storage.read_at(*r.buf, std::uint64_t{first_page} * PAGE_SIZE));
    std::size_t valid_pages = 1;
    std::uint64_t cursor = 0;
    DecodedCuts cuts;
    for (;;) {
        // The length prefix, one byte at a time.
        std::uint64_t len = 0;
        unsigned shift = 0;
        bool torn = false;
        for (;;) {
            std::uint8_t b = 0;
            KLADDE_TRY_ASSIGN(bool ok, r.read(std::span(&b, 1), true));
            if (!ok || shift > 28) {
                torn = true;
                break;
            }
            len |= std::uint64_t{static_cast<std::uint8_t>(b & 0x7f)} << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        if (torn || len > UINT32_MAX) break;
        // The records, read in bounded pieces so that stale bytes claiming a
        // huge length cost nothing before their trailer fails.
        std::vector<std::uint8_t> payload;
        payload.reserve(std::min<std::uint64_t>(len, 1 << 20));
        std::uint64_t left = len;
        while (left > 0) {
            std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(left, 1 << 16));
            std::size_t start = payload.size();
            payload.resize(start + k);
            KLADDE_TRY_ASSIGN(bool ok, r.read(std::span(payload.data() + start, k), true));
            if (!ok) {
                torn = true;
                break;
            }
            left -= k;
        }
        if (torn) break;
        std::uint32_t expected_crc = r.chain.value();
        std::array<std::uint8_t, 4> crc{};
        KLADDE_TRY_ASSIGN(bool ok, r.read(crc, false));
        if (!ok || load_u32(crc.data()) != expected_crc) break;
        std::uint64_t at = out.records.end().at;
        KLADDE_TRY(decode_records(payload, cursor, cuts, out.records));
        out.records.mark_start(at);
        out.transactions += 1;
        valid_pages = r.pages.size();
    }
    out.pages.assign(r.pages.begin(), r.pages.begin() + static_cast<std::ptrdiff_t>(valid_pages));
    return out;
}

}  // namespace kladde::store

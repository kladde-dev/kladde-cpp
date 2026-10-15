#include "journal.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include "util.hpp"

namespace kladde::store {

namespace {

/// What a journal transaction may be off by from the estimate its records'
/// lengths give, since the cursor the estimate assumed may have been reset by
/// a flush in between: a head's delta, at most, grows by this much.
constexpr std::uint64_t ESTIMATE_SLACK = 16;

/// The escape that ends a journal with a seal.
constexpr std::uint64_t ESCAPE_SEAL = 0;
/// The escape that says the next record's bytes are external: in a payload
/// page, named by length, address and checksum.
constexpr std::uint64_t ESCAPE_EXTERNAL = 1;

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
        encode_written(w, cursor, distance, out);
        if (w.cut_len()) cuts.made.push_back(at);
        cursor = w.end();
    }
}

void Records::encode_written(Record const& r, std::uint64_t cursor, std::uint32_t distance,
                             std::vector<std::uint8_t>& out) const {
    if (r.kind != Record::Kind::write && r.kind != Record::Kind::splice)
        return encode_record(r, cursor, distance, *this, out);
    bool is_write = r.kind == Record::Kind::write;
    std::uint64_t removed = is_write ? r.bytes.len : r.removed;
    std::uint64_t start = r.bytes.start, len = r.bytes.len;
    struct Part {
        std::uint64_t start, len;
        std::optional<Placement> placed;
    };
    std::vector<Part> parts;
    std::uint64_t at = start;
    for (auto const& [s, p] : placed_in(start, len)) {
        KLADDE_DEBUG_CHECK(s >= start && s + p.len <= start + len, "a placed run outside its record");
        if (s > at) parts.push_back({at, s - at, std::nullopt});
        parts.push_back({s, p.len, p});
        at = s + p.len;
    }
    if (parts.empty()) return encode_record(r, cursor, distance, *this, out);
    if (at < start + len) parts.push_back({at, start + len - at, std::nullopt});
    std::uint64_t cur = cursor, here = r.pos;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        Part const& part = parts[i];
        Bytes b{part.start, static_cast<std::uint32_t>(part.len)};
        Record rec = i == 0 && is_write && parts.size() == 1 ? Record::write(here, b)
                     : i == 0                                ? Record::splice(here, removed, b)
                                                             : Record::splice(here, 0, b);
        if (!part.placed) {
            encode_record(rec, cur, 0, *this, out);
        } else {
            out.push_back(0x00);
            out.push_back(0x00);
            put_varint(out, ESCAPE_EXTERNAL);
            put_varint(out, head(rec, cur));
            if (rec.kind == Record::Kind::splice) put_varint(out, rec.removed);
            put_varint(out, part.len);
            put_varint(out, part.placed->address());
            auto crc = le_bytes(part.placed->crc);
            out.insert(out.end(), crc.begin(), crc.end());
        }
        here += part.len;
        cur = here;
    }
}

std::vector<std::pair<std::uint64_t, Placement>> Records::placed_in(std::uint64_t start,
                                                                    std::uint64_t len) const {
    std::vector<std::pair<std::uint64_t, Placement>> out;
    auto it = placed_.lower_bound(start);
    if (it != placed_.begin()) {
        auto before = std::prev(it);
        if (before->first + before->second.len > start) out.emplace_back(*before);
    }
    for (; it != placed_.end() && it->first < start + len; ++it) out.emplace_back(*it);
    return out;
}

void Records::truncate(Cursor to) {
    KLADDE_DEBUG_CHECK(front_.at <= to.at && to.at <= end_.at, "truncating outside the buffer");
    records_.resize(to.at - front_.at);
    arena_.resize(arena_skip_ + (to.arena - front_.arena));
    while (!starts_.empty() && starts_.back() >= to.at) starts_.pop_back();
    placed_.erase(placed_.lower_bound(to.arena), placed_.end());
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
    placed_.erase(placed_.begin(), placed_.lower_bound(to.arena));
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

namespace {

expected<bool> decode_into(std::span<std::uint8_t const> input, std::uint64_t& cursor,
                           DecodedCuts& cuts, Records& into, Fetch const& fetch,
                           std::vector<std::size_t>& pasted_now) {
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
        bool external = false;
        if (x == 0 && !rest.empty() && rest[0] == 0) {
            // A `Write` of no bytes at delta 0: the escape.
            rest = rest.subspan(1);
            KLADDE_TRY_ASSIGN(std::uint64_t escape, get());
            if (escape == ESCAPE_SEAL) return corrupt("a seal shares its transaction");
            if (escape != ESCAPE_EXTERNAL) return corrupt("a journal record uses an unknown escape");
            external = true;
            KLADDE_TRY_ASSIGN(std::uint64_t h2, get());
            x = unzigzag(h2);
            if ((x & 3) > 1)
                return corrupt("an escape for external bytes precedes no write or splice");
        }
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
            Bytes bytes;
            if (external) {
                KLADDE_TRY_ASSIGN(std::uint64_t address, get());
                if (rest.size() < 4) return corrupt("a journal record is truncated");
                std::uint32_t crc = load_u32(rest.data());
                rest = rest.subspan(4);
                std::uint64_t page = address / PAGE_SIZE, offset = address % PAGE_SIZE;
                if (len == 0 || len > MAX_PAGE_CONTENT || page < 2 || page >= UINT32_MAX ||
                    offset < CONTENT_OFFSET || offset + len > PAGE_SIZE - 4)
                    return corrupt("an external reference lies outside a data page");
                KLADDE_TRY_ASSIGN(auto got, fetch(address, static_cast<std::uint32_t>(len), crc));
                if (!got) return false;
                bytes = into.stash(got->first);
                into.place(bytes.start, got->second);
            } else {
                if (rest.size() < len) return corrupt("a journal record's bytes are truncated");
                bytes = into.stash(rest.first(len));
                rest = rest.subspan(len);
            }
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
            pasted_now.push_back(i);
            rec = Record::paste(pos, removed, cuts[i].record, cuts[i].len, std::nullopt);
        }
        cursor = rec.end();
        into.push(rec);
    }
    return true;
}

}  // namespace

expected<bool> decode_records(std::span<std::uint8_t const> input, std::uint64_t& cursor,
                              DecodedCuts& cuts, Records& into, Fetch const& fetch) {
    Cursor mark = into.end();
    std::uint64_t cursor0 = cursor;
    std::size_t cuts0 = cuts.size();
    std::vector<std::size_t> pasted_now;
    KLADDE_TRY_ASSIGN(bool whole, decode_into(input, cursor, cuts, into, fetch, pasted_now));
    if (whole) return true;
    into.truncate(mark);
    cursor = cursor0;
    cuts.resize(cuts0);
    for (std::size_t i : pasted_now)
        if (i < cuts.size()) cuts[i].pasted = false;
    return false;
}

// ---------------------------------------------------------------- seals

std::vector<std::uint8_t> seal_payload(std::uint32_t page) {
    std::vector<std::uint8_t> out{0x00, 0x00, 0x00};
    put_varint(out, page);
    return out;
}

std::optional<std::uint32_t> parse_seal(std::span<std::uint8_t const> payload) {
    if (payload.size() < 3 || payload[0] != 0 || payload[1] != 0 || payload[2] != 0)
        return std::nullopt;
    std::span<std::uint8_t const> rest = payload.subspan(3);
    auto page = get_varint(rest);
    if (!page || !rest.empty() || *page > UINT32_MAX) return std::nullopt;
    return static_cast<std::uint32_t>(*page);
}

// ---------------------------------------------------------------- JournalWriter

JournalWriter::JournalWriter(std::uint64_t epoch_, std::uint32_t first_page)
    : pages{first_page}, epoch(epoch_), buf_(new_page()), chain_(seeded_chain(epoch_)) {}

JournalWriter JournalWriter::recovered(std::uint64_t epoch, std::vector<std::uint32_t> pages,
                                       std::vector<std::uint32_t> payload,
                                       std::uint64_t transactions) {
    JournalWriter w(epoch, 0);
    w.pages = std::move(pages);
    w.payload_pages = std::move(payload);
    w.transactions = transactions;
    w.recovered_ = true;
    return w;
}

bool JournalWriter::fits(std::uint64_t n, std::uint32_t budget) const {
    return !recovered_ &&
           held() + framed_len(n) + ESTIMATE_SLACK <= std::uint64_t{budget} * JOURNAL_STREAM;
}

bool JournalWriter::over_budget(std::uint32_t budget) const {
    return held() > std::uint64_t{budget} * JOURNAL_STREAM;
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

/// The payload pages recovery has read, each without bytes if its framing
/// failed, it is no data page, or it lies past the end of the file.
struct PayloadReader {
    struct Read {
        std::unique_ptr<PageBuf> buf;
        std::uint64_t epoch = 0;
        std::size_t start = 0, end = 0;
    };
    Storage const& storage;
    std::uint64_t file_pages;
    std::map<std::uint32_t, std::optional<Read>> pages;

    using Got = std::optional<std::pair<std::vector<std::uint8_t>, Placement>>;

    /// The `len` bytes at `address`, if a `Data` page of epoch `epoch` holds
    /// them in its content and their CRC-32C is `crc`.
    expected<Got> fetch(std::uint64_t epoch, std::uint64_t address, std::uint32_t len,
                        std::uint32_t crc) {
        auto page = static_cast<std::uint32_t>(address / PAGE_SIZE);
        auto offset = static_cast<std::size_t>(address % PAGE_SIZE);
        auto it = pages.find(page);
        if (it == pages.end()) {
            std::optional<Read> read;
            if (page < file_pages) {
                auto buf = new_page();
                KLADDE_TRY(storage.read_at(*buf, std::uint64_t{page} * PAGE_SIZE));
                auto d = decode_page(*buf, false);
                if (auto* dp = std::get_if<DecodedPage>(&d); dp && dp->kind == KIND_DATA)
                    read = Read{std::move(buf), dp->epoch, dp->content_start, dp->content_end};
            }
            it = pages.emplace(page, std::move(read)).first;
        }
        if (!it->second) return Got{};
        Read const& r = *it->second;
        std::size_t end = offset + len;
        if (r.epoch != epoch || offset < r.start || end > r.end) return Got{};
        std::span<std::uint8_t const> bytes(r.buf->data() + offset, len);
        Crc32c c;
        c.update(bytes);
        if (c.value() != crc) return Got{};
        return Got{std::pair{std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
                             Placement{len, page, static_cast<std::uint16_t>(offset), crc, epoch}}};
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
    // The pages of the journals that seals ended, before the current one.
    std::vector<std::uint32_t> sealed_pages;
    std::size_t valid_pages = 1;
    std::uint64_t cursor = 0;
    DecodedCuts cuts;
    PayloadReader payloads{storage, file_pages, {}};
    std::set<std::uint32_t> seen_payload;
    std::uint64_t payload_mark = 0;
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
        if (auto next = parse_seal(payload)) {
            out.transactions += 1;
            // The journal ends here, and the next epoch's begins at `next`,
            // with a chain, a cursor and cuts of its own.
            sealed_pages.insert(sealed_pages.end(), r.pages.begin(), r.pages.end());
            if (*next < 2 || *next >= file_pages ||
                std::find(sealed_pages.begin(), sealed_pages.end(), *next) != sealed_pages.end()) {
                valid_pages = 0;
                break;
            }
            epoch += 1;
            r.chain = seeded_chain(epoch);
            r.pages = {*next};
            r.pos = 0;
            KLADDE_TRY(storage.read_at(*r.buf, std::uint64_t{*next} * PAGE_SIZE));
            valid_pages = 1;
            cursor = 0;
            cuts.clear();
            continue;
        }
        std::uint64_t at = out.records.end().at;
        Fetch fetch = [&](std::uint64_t address, std::uint32_t n, std::uint32_t c) {
            return payloads.fetch(epoch, address, n, c);
        };
        KLADDE_TRY_ASSIGN(bool whole, decode_records(payload, cursor, cuts, out.records, fetch));
        // A payload page's write did not reach the disk: the transaction
        // never happened.
        if (!whole) break;
        out.transactions += 1;
        valid_pages = r.pages.size();
        out.records.mark_start(at);
        std::uint64_t end = out.records.end().arena;
        for (auto const& [s, p] : out.records.placed_in(payload_mark, end - payload_mark)) {
            (void)s;
            if (seen_payload.insert(p.page).second) out.payload_pages.push_back(p.page);
        }
        payload_mark = end;
    }
    out.pages = std::move(sealed_pages);
    out.pages.insert(out.pages.end(), r.pages.begin(),
                     r.pages.begin() + static_cast<std::ptrdiff_t>(valid_pages));
    return out;
}

}  // namespace kladde::store

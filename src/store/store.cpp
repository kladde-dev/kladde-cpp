// The store's public interface, over its inner state, and the write phase:
// recording operations, the transaction and batch state machine, appending
// to the journal, and reading the sequence.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "inner.hpp"
#include "load.hpp"
#include "util.hpp"

namespace kladde {

using namespace store;

static_assert(store::PAGE_SIZE == kladde::page_size);
static_assert(store::MAX_PAGE_CONTENT == kladde::max_page_content);

Store::Inner::Inner(std::unique_ptr<Storage> s, Options o, JournalWriter j)
    : storage(std::move(s)), opts(std::move(o)), journal(std::move(j)) {}

Store::Inner::~Inner() { (void)join_pending(); }

// ---------------------------------------------------------------- opening

expected<Store> Store::create(std::unique_ptr<Storage> storage, Options opts) {
    KLADDE_TRY(storage->resize(0));
    KLADDE_TRY(storage->resize(2 * PAGE_SIZE));
    HeaderFields header;
    header.journal_pointer = 2;
    auto page = new_page();
    std::uint8_t const empty[] = {0};
    encode_page(*page, &header, KIND_TABLE, 0, empty);
    KLADDE_TRY(storage->write_at(*page, 0));
    KLADDE_TRY(storage->sync());
    return open(std::move(storage), std::move(opts));
}

expected<Store> Store::open(std::unique_ptr<Storage> storage, Options opts) {
    KLADDE_TRY_ASSIGN(PickedHeader picked, pick_header(*storage));
    // Retires the older world: after this, every page the governing header
    // does not reach is reusable.
    KLADDE_TRY(storage->sync());
    KLADDE_TRY_ASSIGN(Loaded loaded, load(*storage, std::move(picked)));
    std::uint64_t epoch = loaded.header.epoch;
    std::uint32_t jp = loaded.header.fields.journal_pointer;
    KLADDE_TRY_ASSIGN(Recovered recovered, recover(*storage, epoch + 1, jp));
    std::uint32_t file_pages = loaded.file_pages;
    State& state = loaded.state;
    state.ensure_page(std::max<std::uint32_t>(file_pages > 0 ? file_pages - 1 : 0, 1));
    // The page the header names for the journal is in its world, written to
    // or not, and so is every page the journal went on to.
    std::vector<std::uint32_t> journal_pages = recovered.pages;
    if (jp >= 2 && jp < file_pages) journal_pages.push_back(jp);
    for (std::uint32_t p : journal_pages) {
        Page& page = state.pages[p];
        if (!page.is(Page::State::reusable) && !page.is(Page::State::journal))
            return corrupt("journal page " + std::to_string(p) + " is in the table's world");
        page = Page::journal();
    }
    // A non-empty recovered journal is folded below, before anything can be
    // appended to it; an empty one is appended to from its start.
    bool fold_recovered = recovered.transactions > 0;
    JournalWriter journal = fold_recovered ? JournalWriter::recovered(epoch + 1, recovered.pages,
                                                                      recovered.transactions)
                                           : JournalWriter(epoch + 1, jp);
    auto inner = std::make_unique<Inner>(std::move(storage), std::move(opts), std::move(journal));
    for (std::uint32_t p = 2; p < file_pages; ++p)
        if (state.pages[p].is(Page::State::reusable)) inner->ready.insert(p);
    inner->epoch = epoch;
    inner->header = loaded.header.fields;
    inner->header_slot = loaded.header.slot;
    inner->len = state.seq_len();
    inner->state = std::move(loaded.state);
    inner->table_pages = std::move(loaded.table_pages);
    inner->file_pages = file_pages;
    inner->schema = std::move(loaded.schema);
    inner->private_ = std::move(loaded.private_);
    // All of it in the journal pages already, as `immediate` implies.
    inner->ops = std::move(recovered.records);
    inner->tx = Tx::immediate();
    inner->mirror = std::move(loaded.mirror);
    inner->cons.budget = inner->opts.budget_pages;
    // Replay: a recovered journal is folded now, so that what the application
    // decodes sees the state at its last transaction.
    if (fold_recovered) {
        KLADDE_TRY_ASSIGN(inner->len, inner->replayed_len());
        KLADDE_TRY(inner->flush_now());
        KLADDE_TRY(inner->load_mirror());
    }
    return Store(std::move(inner));
}

Store::Store(std::unique_ptr<Inner> inner) : inner_(std::move(inner)) {}
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;
Store::~Store() = default;

// ---------------------------------------------------------------- flushing

expected<void> Store::flush() {
    if (inner_->poisoned) return errc::poisoned;
    return inner_->flush_now();
}

expected<bool> Store::flush_if_journal_exceeds(double fraction) {
    if (inner_->poisoned) return errc::poisoned;
    return inner_->flush_if_journal_exceeds(fraction);
}

expected<void> Store::close() {
    if (inner_->poisoned) return errc::poisoned;
    KLADDE_TRY(inner_->flush_now());
    return inner_->truncate(true);
}

// ---------------------------------------------------------------- the small chains

std::vector<std::uint8_t> Store::schema() const {
    return inner_->schema_pending ? *inner_->schema_pending : inner_->schema;
}

void Store::set_schema(std::vector<std::uint8_t> bytes) { inner_->schema_pending = std::move(bytes); }

std::vector<std::uint8_t> Store::private_state() const {
    return inner_->private_pending ? *inner_->private_pending : inner_->private_;
}

void Store::set_private_state(std::vector<std::uint8_t> bytes) {
    inner_->private_pending = std::move(bytes);
}

std::array<std::uint8_t, 16> Store::root_fingerprint() const {
    return inner_->header.root_fingerprint;
}

void Store::set_root_fingerprint(std::array<std::uint8_t, 16> fingerprint) {
    inner_->header.root_fingerprint = fingerprint;
}

// ---------------------------------------------------------------- reading

expected<void> Store::read(std::function<bool(std::span<std::uint8_t const>)> const& chunk) const {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    KLADDE_TRY(i.load_mirror());
    static PageBuf const zeros{};
    for (std::uint32_t node = i.state.rope.first(i.state.root); node != NIL;
         node = i.state.rope.next(node)) {
        RopeItem item = i.state.rope.item(node);
        KLADDE_CHECK(!item.lit, "a literal in the rope between flushes");
        Span const& s = i.state.span(item.id_or_start);
        switch (s.content.kind) {
            case Content::Kind::ref: {
                auto it = i.mirror.find(s.content.page);
                KLADDE_CHECK(it != i.mirror.end(), "the mirror lacks a page the sequence shows");
                if (!chunk(std::span(it->second->data() + s.content.offset, item.len))) return {};
                break;
            }
            case Content::Kind::inline_:
                if (!chunk(std::span(i.table_pages.at(s.content.page)->data() + s.content.offset,
                                     item.len)))
                    return {};
                break;
            case Content::Kind::zero:
                for (std::uint64_t left = item.len; left > 0;) {
                    std::size_t n = std::min<std::uint64_t>(left, PAGE_SIZE);
                    if (!chunk(std::span(zeros.data(), n))) return {};
                    left -= n;
                }
                break;
            default: KLADDE_CHECK(false, "a span shows a pending or free content between flushes");
        }
    }
    return {};
}

expected<std::vector<std::uint8_t>> Store::read_all() const {
    std::vector<std::uint8_t> out;
    KLADDE_TRY(read([&](std::span<std::uint8_t const> c) {
        out.insert(out.end(), c.begin(), c.end());
        return true;
    }));
    return out;
}

void Store::end_load() { inner_->mirror.clear(); }

Stats Store::stats() const {
    Stats s = inner_->stats;
    inner_->fill_stats(s);
    return s;
}

bool Store::is_poisoned() const { return inner_->poisoned; }

void Store::poison() { inner_->poisoned = true; }

void Store::check() const {
    Inner const& i = *inner_;
    i.state.check(i.header_slot);
    KLADDE_CHECK(i.header.sequence_head == i.state.seq_head(), "the header's sequence head");
    KLADDE_CHECK(i.header.schema_head == (i.state.schema.empty() ? 0 : i.state.schema.front()),
                 "the header's schema head");
}

std::string Store::describe() const {
    Inner const& i = *inner_;
    std::ostringstream out;
    static char const* const kinds[] = {"free", "ref", "inline", "zero", "pending"};
    for (std::uint32_t id : i.state.seq_spans()) {
        Span const& s = i.state.span(id);
        out << "span " << id << ": " << s.len << " bytes "
            << kinds[static_cast<int>(s.content.kind)] << "(" << s.content.page << ", "
            << s.content.offset << "), next " << s.next << ", entry in " << s.entry_page << " ("
            << s.entry_len << " bytes), heat " << s.heat << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------- transactions and batches

expected<void> Store::begin_transaction() {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    Tx t = i.tx;
    switch (t.kind) {
        case Tx::Kind::immediate: i.tx = Tx::in_transaction(1, 0, i.ops.end()); break;
        case Tx::Kind::in_batch: i.tx = Tx::in_tx_in_batch(1, t.batch, t.committed, i.ops.end()); break;
        case Tx::Kind::in_transaction:
        case Tx::Kind::in_tx_in_batch: i.tx.depth += 1; break;
    }
    return {};
}

expected<void> Store::end_transaction() {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    Tx t = i.tx;
    if (t.kind == Tx::Kind::in_transaction) {
        if (t.depth > 1) {
            i.tx.depth -= 1;
        } else if (t.batch == 0) {
            KLADDE_TRY(i.close(t.committed, t.committed, false));
            i.tx = Tx::immediate();
        } else {
            return corrupt("a batch opened inside a transaction is still open");
        }
    } else if (t.kind == Tx::Kind::in_tx_in_batch) {
        if (t.depth > 1) {
            i.tx.depth -= 1;
        } else {
            KLADDE_TRY_ASSIGN(Cursor committed, i.close(t.committed, t.ready, true));
            i.tx = Tx::in_batch(t.batch, committed);
        }
    } else {
        return corrupt("no transaction is open");
    }
    return {};
}

void Store::abandon_transaction() {
    Inner& i = *inner_;
    Cursor keep = i.tx.kind == Tx::Kind::in_tx_in_batch    ? i.tx.ready
                  : i.tx.kind == Tx::Kind::in_transaction ? i.tx.committed
                                                          : i.ops.end();
    i.truncate_ops(keep);
    i.poisoned = true;
}

expected<void> Store::begin_batch() {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    if (i.tx.kind == Tx::Kind::immediate)
        i.tx = Tx::in_batch(1, i.ops.end());
    else
        i.tx.batch += 1;
    return {};
}

expected<void> Store::end_batch() {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    Tx t = i.tx;
    if (t.kind == Tx::Kind::in_batch && t.batch == 1) {
        KLADDE_TRY(i.append(t.committed, i.ops.end()));
        if (i.journal.over_budget(i.opts.journal_budget_pages)) KLADDE_TRY(i.fold_journal(i.ops.end()));
        i.tx = Tx::immediate();
    } else if ((t.kind == Tx::Kind::in_batch && t.batch > 1) ||
               (t.kind == Tx::Kind::in_transaction && t.batch > 0) ||
               (t.kind == Tx::Kind::in_tx_in_batch && t.batch > 1)) {
        i.tx.batch -= 1;
    } else {
        return corrupt("no batch is open, or a transaction inside it is");
    }
    return {};
}

expected<void> Store::begin_atomically() {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    AtomicMark mark{i.ops.end(), i.tx, i.len};
    KLADDE_TRY(begin_transaction());
    i.atomic_marks.push_back(mark);
    return {};
}

expected<void> Store::end_atomically() {
    inner_->atomic_marks.pop_back();
    return end_transaction();
}

void Store::roll_back_atomically() {
    Inner& i = *inner_;
    AtomicMark mark = i.atomic_marks.back();
    i.atomic_marks.pop_back();
    if (i.poisoned) return;
    // Inside a transaction nothing past `mark` is appended, so moving the
    // buffer's end back to it undoes what was recorded as far as the file is
    // concerned.
    i.truncate_ops(mark.end);
    Cursor committed = i.tx.kind == Tx::Kind::immediate ? mark.end : i.tx.committed;
    i.tx = mark.tx.with_committed(committed);
    i.len = mark.len;
}

// ---------------------------------------------------------------- mutations

std::uint64_t Store::size() const { return inner_->len; }

expected<std::optional<CutId>> Store::write(std::uint64_t pos, std::span<std::uint8_t const> bytes) {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    if (bytes.empty()) return std::optional<CutId>{};
    KLADDE_TRY(i.check_range(pos, bytes.size()));
    if (bytes.size() > UINT32_MAX) return errc::out_of_bounds;
    Bytes stashed = i.ops.stash(bytes);
    CutId cut = i.new_cut(stashed.len);
    KLADDE_TRY(i.record(Record::write(pos, stashed)));
    return std::optional<CutId>(cut);
}

expected<std::optional<CutId>> Store::splice(std::uint64_t pos, std::uint64_t removed,
                                             std::span<std::uint8_t const> bytes) {
    if (removed == bytes.size()) return write(pos, bytes);
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    KLADDE_TRY(i.check_range(pos, removed));
    KLADDE_TRY_ASSIGN(std::uint64_t len, i.grown(removed, bytes.size()));
    if (bytes.size() > UINT32_MAX) return errc::out_of_bounds;
    Bytes stashed = i.ops.stash(bytes);
    std::optional<CutId> cut;
    if (removed > 0) cut = i.new_cut(removed);
    i.len = len;
    KLADDE_TRY(i.record(Record::splice(pos, removed, stashed)));
    return cut;
}

expected<void> Store::move(std::uint64_t from, std::uint64_t len, std::uint64_t to) {
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    KLADDE_TRY(i.check_range(from, len));
    if (to > i.len - len) return errc::out_of_bounds;
    if (len == 0 || from == to) return {};
    return i.record(Record::move(from, len, to));
}

expected<void> Store::paste(std::uint64_t pos, std::uint64_t removed, CutId cut,
                            std::span<std::uint8_t const> bytes) {
    if (bytes.size() != cut.length) return corrupt("a paste's bytes are not as long as its cut");
    if (!is_alive(cut)) {
        // Its journal is folded: what it held is the value's encoding,
        // written anew.
        KLADDE_TRY(splice(pos, removed, bytes));
        return {};
    }
    Inner& i = *inner_;
    if (i.poisoned) return errc::poisoned;
    if (i.pasted.contains(cut.record)) return corrupt("a cut is pasted twice");
    Record const* r = i.ops.get(cut.record);
    if (!r || r->cut_len() != cut.length) return corrupt("no such cut");
    KLADDE_TRY(i.check_range(pos, removed));
    KLADDE_TRY_ASSIGN(std::uint64_t len, i.grown(removed, cut.length));
    if (bytes.size() > UINT32_MAX) return errc::out_of_bounds;
    Bytes fallback = i.ops.stash(bytes);
    i.pasted[cut.record] = i.ops.end().at;
    i.len = len;
    return i.record(Record::paste(pos, removed, cut.record, cut.length, fallback));
}

bool Store::is_alive(CutId const& cut) const { return cut.record >= inner_->ops.front().at; }

// ---------------------------------------------------------------- Inner: recording

expected<void> Store::Inner::record(Record const& r) {
    Cursor start = ops.end();
    ops.push(r);
    switch (tx.kind) {
        case Tx::Kind::immediate: {
            KLADDE_TRY(close(start, start, false));
            return {};
        }
        case Tx::Kind::in_batch: {
            KLADDE_TRY_ASSIGN(Cursor committed, close(tx.committed, start, true));
            tx = Tx::in_batch(tx.batch, committed);
            return {};
        }
        default: return {};
    }
}

CutId Store::Inner::new_cut(std::uint64_t n) const { return CutId{ops.end().at, n}; }

void Store::Inner::truncate_ops(Cursor to) {
    ops.truncate(to);
    std::erase_if(pasted, [&](auto const& kv) { return kv.second >= to.at; });
}

expected<Cursor> Store::Inner::close(Cursor committed, Cursor ready_at, bool in_batch) {
    std::uint32_t budget = opts.journal_budget_pages;
    Cursor end = ops.end();
    if (ready_at == end) return committed;
    if (!journal.fits(end.bytes - committed.bytes, budget)) {
        KLADDE_TRY_ASSIGN(committed, append(committed, ready_at));
        if (!journal.is_empty()) KLADDE_TRY(fold_journal(committed));
    }
    if (in_batch && journal.fits(end.bytes - committed.bytes, budget)) return committed;
    KLADDE_TRY_ASSIGN(committed, append(committed, end));
    if (journal.over_budget(budget)) KLADDE_TRY(fold_journal(committed));
    return committed;
}

expected<Cursor> Store::Inner::append(Cursor from, Cursor to) {
    if (from == to) return to;
    KLADDE_TRY(ensure_journal_page());
    std::vector<std::uint8_t> payload;
    payload.reserve(to.bytes - from.bytes);
    std::uint64_t cursor = journal.cursor;
    // The journal's cuts take the transaction's in place: copying them would
    // make every append cost as much as the journal has cuts. A failure from
    // here on poisons the store, which then needs them no more.
    ops.encode(from, to, cursor, journal_cuts, payload);
    ops.mark_start(from.at);
    std::uint64_t before = journal.bytes;
    auto r = journal.append(payload, *storage,
                            [this](Storage&) { return take_page(Page::journal()); });
    if (!r) {
        poisoned = true;
        return std::move(r).error();
    }
    journal.cursor = cursor;
    stats.journal_bytes += journal.bytes - before;
    stats.transactions += 1;
    return to;
}

expected<void> Store::Inner::ensure_journal_page() {
    std::uint32_t p = journal.current_page();
    if (p >= file_pages) {
        KLADDE_TRY(storage->resize((std::uint64_t{p} + 1) * PAGE_SIZE));
        for (std::uint32_t q = file_pages; q < p; ++q) {
            state.ensure_page(q);
            if (q >= 2 && state.pages[q].is(Page::State::reusable)) ready.insert(q);
        }
        file_pages = p + 1;
        state.ensure_page(p);
    }
    ready.erase(p);
    state.pages[p] = Page::journal();
    return {};
}

expected<std::uint64_t> Store::Inner::replayed_len() const {
    std::uint64_t n = state.seq_len();
    auto past_end = [] { return corrupt("a journal record reaches past the end of the sequence"); };
    for (Record const& r : ops.all()) {
        switch (r.kind) {
            case Record::Kind::write:
            case Record::Kind::move: break;
            case Record::Kind::splice:
                if (n < r.removed) return past_end();
                n = n - r.removed + r.bytes.len;
                break;
            case Record::Kind::paste:
                if (n < r.removed) return past_end();
                n = n - r.removed + r.len;
                break;
        }
    }
    return n;
}

// ---------------------------------------------------------------- Inner: reading

expected<void> Store::Inner::load_mirror() {
    std::vector<std::uint32_t> want;
    for (auto const& [p, list] : state.data_spans) {
        (void)list;
        if (!mirror.contains(p)) want.push_back(p);
    }
    std::sort(want.begin(), want.end());
    for (std::uint32_t p : want) {
        auto buf = new_page();
        KLADDE_TRY(storage->read_at(*buf, std::uint64_t{p} * PAGE_SIZE));
        if (!std::holds_alternative<DecodedPage>(decode_page(*buf, false)))
            return corrupt("data page " + std::to_string(p) + " fails its checksum");
        stats.pages_read += 1;
        mirror[p] = std::move(buf);
    }
    return {};
}

expected<PageBuf const*> Store::Inner::page_bytes(std::uint32_t page) {
    if (auto it = table_pages.find(page); it != table_pages.end()) return it->second.get();
    if (auto it = mirror.find(page); it != mirror.end()) return it->second.get();
    auto it = cache.find(page);
    if (it == cache.end()) {
        auto buf = new_page();
        KLADDE_TRY(storage->read_at(*buf, std::uint64_t{page} * PAGE_SIZE));
        stats.pages_read += 1;
        it = cache.emplace(page, std::move(buf)).first;
    }
    return it->second.get();
}

expected<void> Store::Inner::check_range(std::uint64_t pos, std::uint64_t n) const {
    if (pos > len || n > len - pos) return errc::out_of_bounds;
    return {};
}

expected<std::uint64_t> Store::Inner::grown(std::uint64_t removed, std::uint64_t added) const {
    std::uint64_t n = len - removed;
    if (added >= MAX_SEQUENCE || n + added >= MAX_SEQUENCE) return errc::out_of_bounds;
    return n + added;
}

expected<std::uint32_t> Store::Inner::take_page(Page page) {
    std::uint32_t p;
    if (!ready.empty()) {
        p = *ready.begin();
        ready.erase(ready.begin());
    } else {
        p = file_pages;
        if (p == UINT32_MAX) return errc::out_of_bounds;
        KLADDE_TRY(storage->resize((std::uint64_t{p} + 1) * PAGE_SIZE));
        file_pages = p + 1;
        state.ensure_page(p);
        stats.file_grew += 1;
    }
    KLADDE_DEBUG_CHECK(state.pages[p].is(Page::State::reusable), "taking a page that is not reusable");
    state.pages[p] = page;
    return p;
}

}  // namespace kladde

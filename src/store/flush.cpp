// The flush: fold the journal, map the new sequence onto spans, choose which
// entries the header keeps and merge short spans as theirs leave it, place
// new bytes in data pages, encode the span table, and commit.
#include <algorithm>
#include <cstring>
#include <deque>
#include <string>
#include <tuple>

#include "inner.hpp"
#include "util.hpp"

namespace kladde {

using namespace store;

namespace {

/// The largest run of bytes a span shows from a data page.
constexpr std::uint32_t C = MAX_PAGE_CONTENT;

/// The cursors a state implies for its committed run and a batch's ready run.
std::pair<Cursor, Cursor> committed_and_ready(Tx const& tx, Cursor end) {
    switch (tx.kind) {
        case Tx::Kind::immediate: return {end, end};
        case Tx::Kind::in_batch: return {tx.committed, end};
        case Tx::Kind::in_transaction: return {tx.committed, tx.committed};
        case Tx::Kind::in_tx_in_batch: return {tx.committed, tx.ready};
    }
    return {end, end};
}

std::vector<std::uint32_t> sorted(IdSet const& s) {
    std::vector<std::uint32_t> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    return v;
}

}  // namespace

expected<void> Store::Inner::flush_now() {
    if (poisoned) return errc::poisoned;
    auto [committed, ready_at] = committed_and_ready(tx, ops.end());
    KLADDE_TRY_ASSIGN(Cursor c, append(committed, ready_at));
    tx = tx.with_committed(c);
    // An explicit flush returns once what it folded is durable.
    explicit_flush = true;
    auto r = fold_journal(c);
    explicit_flush = false;
    KLADDE_TRY(r);
    return join_pending();
}

expected<void> Store::Inner::join_pending() {
    if (!pending) return {};
    PendingCommit p = std::move(*pending);
    pending.reset();
    p.worker.join();
    if (!*p.result) {
        poisoned = true;
        return p.result->error();
    }
    for (std::uint32_t page : p.release) {
        KLADDE_DEBUG_CHECK(state.pages[page].is(Page::State::fallback), "a released page is not fallback");
        state.pages[page] = Page::reusable();
        ready.insert(page);
    }
    return {};
}

expected<void> Store::Inner::seal() {
    // Without a reusable page it does not seal: a page added at the end now
    // would lie below every page the flush adds after it, and keep the file
    // from being truncated to them.
    if (ready.empty()) return {};
    // The page is taken now, so that the flush's own pages cannot be it, and
    // a session's first flush clears it before the seal names it
    // (spec/journal.md#the-start-of-a-session).
    std::uint32_t jp = *ready.begin();
    ready.erase(ready.begin());
    if (first_flush) {
        // Synced before the seal can name it: write-back is unordered, and a
        // seal that reached the disk without the zeros would lead recovery
        // into whatever a lost header's journal left in the page, under the
        // epoch this session re-issues.
        KLADDE_TRY(storage->write_at(*new_page(), std::uint64_t{jp} * PAGE_SIZE));
        KLADDE_TRY(storage->sync());
    }
    state.pages[jp] = Page::journal();
    std::uint64_t before = journal.bytes;
    KLADDE_TRY(journal.append(seal_payload(jp), *storage,
                              [this](Storage&) { return take_page(Page::journal()); }));
    stats.journal_bytes += journal.bytes - before;
    sealed_next = jp;
    return {};
}

expected<bool> Store::Inner::flush_if_journal_exceeds(double fraction) {
    if (poisoned) return errc::poisoned;
    auto [committed, ready_at] = committed_and_ready(tx, ops.end());
    std::uint64_t held = journal.bytes + (ready_at.bytes - committed.bytes);
    double budget = double(opts.journal_budget(len)) * double(JOURNAL_STREAM);
    if (double(held) < fraction * budget) return false;
    KLADDE_TRY(flush_now());
    return true;
}

expected<void> Store::Inner::fold_journal(Cursor committed) {
    if (poisoned) return errc::poisoned;
    auto r = flush_inner(committed);
    if (!r) {
        poisoned = true;
        return r;
    }
    // The next journal starts at `committed`; the cuts of the folded one have
    // ended.
    ops.discard_to(committed);
    journal_cuts = JournalCuts::starting_at(committed.at);
    std::erase_if(pasted, [&](auto const& kv) { return kv.first < committed.at; });
    return {};
}

expected<void> Store::Inner::flush_inner(Cursor committed) {
    // One commit at a time: the pages the last one freed become reusable only
    // through its `fsync`.
    KLADDE_TRY(join_pending());
    if (opts.seal && !journal.is_recovered() && !journal.is_empty()) KLADDE_TRY(seal());
    std::uint64_t e = epoch + 1;
    state.flush_epoch = e;
    Flush f;
    begin_consolidation();

    // 1. The fold, and the new sequence mapped onto spans.
    std::uint64_t n0 = state.seq_len();
    std::uint64_t first = ops.front().at;
    KLADDE_TRY_ASSIGN(auto pieces, fold(ops, ops.front(), committed, n0, first));
    KLADDE_TRY(apply_pieces(pieces, f));
    KLADDE_TRY(write_small_chains(f));

    // 2. The header's entries, and merges of short spans as theirs leave it;
    //    then the rotating walk's merges of cold spans.
    choose_header(f);
    KLADDE_TRY(merge_evicted(f));
    if (opts.consolidate) KLADDE_TRY(walk(f));

    // 3. Data pages, with victims riding along, then budgeted pages.
    Output out;
    KLADDE_TRY(pack(f, out));
    if (opts.consolidate) KLADDE_TRY(budget_loop(f, out));

    // 4. The span table.
    KLADDE_TRY(cut(f, out));

    // 5. Write, fsync, commit.
    return commit(e, std::move(out));
}

// ---------------------------------------------------------------- spans

void Store::Inner::kill_in(Flush& f, std::uint32_t id) {
    state.kill(id);
    f.stated.erase(id);
    f.header.erase(id);
}

expected<std::uint32_t> Store::Inner::span_to_scratch(Flush& f, std::uint32_t id) {
    auto start = static_cast<std::uint32_t>(f.scratch.size());
    Span s = state.span(id);
    std::size_t n = s.len;
    switch (s.content.kind) {
        case Content::Kind::ref:
        case Content::Kind::inline_: {
            KLADDE_TRY_ASSIGN(PageBuf const* buf, page_bytes(s.content.page));
            auto from = buf->begin() + s.content.offset;
            f.scratch.insert(f.scratch.end(), from, from + static_cast<std::ptrdiff_t>(n));
            break;
        }
        case Content::Kind::zero: f.scratch.resize(f.scratch.size() + n, 0); break;
        case Content::Kind::pending: {
            std::size_t p = f.pending[s.content.page].start;
            f.scratch.resize(f.scratch.size() + n);
            std::memmove(f.scratch.data() + start, f.scratch.data() + p, n);
            break;
        }
        case Content::Kind::free: return corrupt("reading free span " + std::to_string(id));
    }
    return start;
}

expected<std::vector<std::uint32_t>> Store::Inner::new_spans(Flush& f, std::uint32_t start,
                                                              std::uint32_t total, Chain chain,
                                                              std::uint64_t heat,
                                                              std::uint32_t after) {
    std::uint32_t inline_max = std::min<std::uint32_t>(opts.inline_threshold, MAX_INLINE);
    std::vector<std::uint32_t> out;
    std::uint32_t at = 0;
    std::uint32_t prev = after;
    while (at < total) {
        std::uint32_t n = total <= inline_max ? total : std::min(total - at, C);
        auto bytes = f.scratch.begin() + start + at;
        bool zeros = n >= 32 && std::all_of(bytes, bytes + n, [](std::uint8_t b) { return b == 0; });
        Content content;
        if (zeros) {
            content = Content::zero();
        } else {
            f.pending.push_back({start + at, total <= inline_max});
            content = Content::pending(static_cast<std::uint32_t>(f.pending.size() - 1));
        }
        auto id = state.new_span(prev);
        if (!id) return errc::out_of_bounds;
        Span& s = state.span_mut(*id);
        s = Span{};
        s.len = n;
        s.content = content;
        s.chain = chain;
        s.entry_page = NONE;
        s.heat = heat;
        s.node = NIL;
        if (!out.empty()) state.span_mut(out.back()).next = *id;
        f.stated.insert(*id);
        out.push_back(*id);
        prev = *id;
        at += n;
    }
    return out;
}

// ---------------------------------------------------------------- mapping

expected<void> Store::Inner::apply_pieces(std::vector<Piece> const& pieces, Flush& f) {
    std::uint64_t n0 = state.seq_len();
    bool unchanged = pieces.empty() ? n0 == 0
                     : pieces.size() == 1
                         ? !pieces[0].arena && pieces[0].start == 0 && pieces[0].len == n0
                         : false;
    if (unchanged) return {};
    std::vector<std::uint64_t> cuts;
    for (Piece const& p : pieces) {
        if (!p.arena) {
            cuts.push_back(p.start);
            cuts.push_back(p.start + p.len);
        }
    }
    std::erase_if(cuts, [&](std::uint64_t x) { return x == 0 || x >= n0; });
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    for (std::uint64_t x : cuts) KLADDE_TRY(boundary(x, f));
    // The committed sequence, cut into pieces at those positions: where each
    // starts, its tree, and whether a piece of the new sequence shows it.
    struct Seg {
        std::uint64_t start;
        std::uint32_t tree;
        bool used;
    };
    std::vector<Seg> segs;
    if (n0 > 0) {
        std::uint32_t rest = state.root;
        std::uint64_t prev = 0;
        cuts.push_back(n0);
        for (std::uint64_t x : cuts) {
            auto [seg, r] = state.rope.split(rest, x - prev);
            segs.push_back({prev, seg, false});
            rest = r;
            prev = x;
        }
        KLADDE_DEBUG_CHECK(rest == NIL, "the committed sequence outlasts its cuts");
    }
    std::uint32_t root = NIL;
    std::vector<std::uint32_t> touched;
    for (Piece const& p : pieces) {
        if (!p.arena) {
            auto it = std::lower_bound(segs.begin(), segs.end(), p.start,
                                       [](Seg const& s, std::uint64_t v) { return s.start < v; });
            if (it == segs.end() || it->start != p.start)
                return corrupt("a piece starts inside a segment");
            std::uint32_t seg = it->tree;
            KLADDE_DEBUG_CHECK(state.rope.sum(seg) == p.len, "a piece and its segment differ");
            it->used = true;
            touched.push_back(state.rope.first(seg));
            touched.push_back(state.rope.last(seg));
            root = state.rope.merge(root, seg);
        } else {
            std::uint64_t done = 0;
            while (done < p.len) {
                std::uint64_t n = std::min<std::uint64_t>(p.len - done, std::uint64_t{1} << 30);
                std::size_t start = f.scratch.size();
                f.scratch.resize(start + n);
                ops.copy_bytes(p.start + done, std::span(f.scratch.data() + start, n));
                stats.fresh_bytes += n;
                std::uint32_t node = state.rope.node(RopeItem::literal(
                    static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(n)));
                touched.push_back(node);
                root = state.rope.merge(root, node);
                done += n;
            }
        }
    }
    for (Seg const& s : segs) {
        if (s.used) continue;
        for (std::uint32_t n : state.rope.nodes(s.tree)) {
            RopeItem it = state.rope.item(n);
            if (!it.lit) kill_in(f, it.id_or_start);
        }
        state.rope.free_tree(s.tree);
    }
    state.root = root;
    return settle(f, touched);
}

expected<void> Store::Inner::boundary(std::uint64_t x, Flush& f) {
    for (;;) {
        auto found = state.rope.find(state.root, x);
        if (!found) return {};
        auto [node, off] = *found;
        if (off == 0) return {};
        auto off32 = static_cast<std::uint32_t>(off);
        RopeItem item = state.rope.item(node);
        if (item.lit) {
            state.rope.set_item(node, RopeItem::literal(item.id_or_start, off32));
            std::uint32_t m =
                state.rope.node(RopeItem::literal(item.id_or_start + off32, item.len - off32));
            insert_at(x, m);
            return {};
        }
        std::uint32_t id = item.id_or_start;
        std::uint32_t span_len = item.len;
        Span s = state.span(id);
        bool dissolve = s.content.kind == Content::Kind::inline_ ||
                        (s.content.kind == Content::Kind::ref && span_len < opts.rewrite_below);
        if (dissolve) {
            KLADDE_TRY_ASSIGN(std::uint32_t start, span_to_scratch(f, id));
            stats.rewritten_bytes += span_len;
            kill_in(f, id);
            state.rope.set_item(node, RopeItem::literal(start, span_len));
            continue;
        }
        Content content;
        if (s.content.kind == Content::Kind::ref)
            content = Content::ref(s.content.page, static_cast<std::uint16_t>(s.content.offset + off));
        else if (s.content.kind == Content::Kind::zero)
            content = Content::zero();
        else
            KLADDE_CHECK(false, "splitting a span that shows neither a page's bytes nor zeros");
        auto t = state.new_span(id);
        if (!t) return errc::out_of_bounds;
        Span& ts = state.span_mut(*t);
        ts = Span{};
        ts.len = span_len - off32;
        ts.next = s.next;
        ts.content = content;
        ts.chain = Chain::sequence;
        ts.entry_page = NONE;
        ts.heat = s.heat;
        ts.node = NIL;
        state.span_mut(id).len = off32;
        state.span_mut(id).next = *t;
        state.note_data(*t);
        state.rope.set_item(node, RopeItem::span(id, off32));
        std::uint32_t m = state.rope.node(RopeItem::span(*t, span_len - off32));
        state.span_mut(*t).node = m;
        insert_at(x, m);
        f.stated.insert(id);
        f.stated.insert(*t);
        stats.span_splits += 1;
        return {};
    }
}

void Store::Inner::insert_at(std::uint64_t x, std::uint32_t m) {
    auto& rope = state.rope;
    auto [l, r] = rope.split(state.root, x);
    std::uint32_t lm = rope.merge(l, m);
    state.root = rope.merge(lm, r);
}

expected<void> Store::Inner::settle(Flush& f, std::vector<std::uint32_t> const& touched) {
    std::uint64_t e = state.flush_epoch;
    IdSet gone;
    std::vector<std::uint32_t> link;
    auto& rope = state.rope;
    auto is_lit = [&](std::uint32_t m) { return m != NIL && rope.item(m).lit; };
    for (std::uint32_t n : touched) {
        if (n == NIL || gone.contains(n)) continue;
        if (!rope.item(n).lit) {
            link.push_back(n);
            continue;
        }
        std::uint32_t a = n;
        while (is_lit(rope.prev(a))) a = rope.prev(a);
        std::vector<std::uint32_t> run;
        for (std::uint32_t m = a; is_lit(m); m = rope.next(m)) run.push_back(m);
        std::uint64_t pos = rope.pos_of(a);
        // The run's bytes, contiguous in the scratch.
        std::uint64_t total = 0;
        bool contiguous = true;
        std::optional<std::uint32_t> expect;
        for (std::uint32_t r : run) {
            RopeItem it = rope.item(r);
            if (expect && *expect != it.id_or_start) contiguous = false;
            expect = it.id_or_start + it.len;
            total += it.len;
        }
        std::uint32_t start;
        if (contiguous) {
            start = rope.item(a).id_or_start;
        } else {
            start = static_cast<std::uint32_t>(f.scratch.size());
            for (std::uint32_t r : run) {
                RopeItem it = rope.item(r);
                std::size_t at = f.scratch.size();
                f.scratch.resize(at + it.len);
                std::memmove(f.scratch.data() + at, f.scratch.data() + it.id_or_start, it.len);
            }
        }
        for (std::uint32_t r : run) gone.insert(r);
        auto [l, rest] = rope.split(state.root, pos);
        auto [mid, r] = rope.split(rest, total);
        rope.free_tree(mid);
        state.root = rope.merge(l, r);
        if (total > UINT32_MAX) return errc::out_of_bounds;
        KLADDE_TRY_ASSIGN(auto ids, new_spans(f, start, static_cast<std::uint32_t>(total),
                                              Chain::sequence, e, 0));
        std::uint64_t at = pos;
        for (std::uint32_t id : ids) {
            std::uint32_t slen = state.span(id).len;
            std::uint32_t node = rope.node(RopeItem::span(id, slen));
            state.span_mut(id).node = node;
            gone.erase(node);
            insert_at(at, node);
            at += slen;
            link.push_back(node);
        }
    }
    for (std::uint32_t n : link) {
        if (gone.contains(n)) continue;
        std::uint32_t p = rope.prev(n);
        for (std::uint32_t m : {p, n}) {
            if (m == NIL) continue;
            std::uint32_t id = state.span_at(m);
            std::uint32_t k = rope.next(m);
            std::uint32_t nx = k == NIL ? 0 : state.span_at(k);
            if (state.span(id).next != nx) {
                state.span_mut(id).next = nx;
                f.stated.insert(id);
            }
        }
    }
    return {};
}

expected<void> Store::Inner::write_small_chains(Flush& f) {
    std::uint64_t e = state.flush_epoch;
    for (Chain chain : {Chain::schema, Chain::private_}) {
        auto& content = chain == Chain::schema ? schema_pending : private_pending;
        if (!content) continue;
        std::vector<std::uint8_t> bytes = std::move(*content);
        content.reset();
        auto& list = chain == Chain::schema ? state.schema : state.private_;
        std::vector<std::uint32_t> old = std::move(list);
        list.clear();
        for (std::uint32_t id : old) kill_in(f, id);
        auto start = static_cast<std::uint32_t>(f.scratch.size());
        f.scratch.insert(f.scratch.end(), bytes.begin(), bytes.end());
        std::vector<std::uint32_t> ids;
        if (!bytes.empty()) {
            if (bytes.size() > UINT32_MAX) return errc::out_of_bounds;
            KLADDE_TRY_ASSIGN(ids, new_spans(f, start, static_cast<std::uint32_t>(bytes.size()),
                                             chain, e, 0));
        }
        list = std::move(ids);
        (chain == Chain::schema ? schema : private_) = std::move(bytes);
    }
    return {};
}

// ---------------------------------------------------------------- the header

Body Store::Inner::body(Flush const& f, std::uint32_t id) const {
    Span const& s = state.span(id);
    switch (s.content.kind) {
        case Content::Kind::ref:
            return Body::ref(s.len, std::uint64_t{s.content.page} * PAGE_SIZE + s.content.offset);
        case Content::Kind::zero: return Body::zero(s.len);
        case Content::Kind::inline_:
            return Body::inline_(
                std::span(table_pages.at(s.content.page)->data() + s.content.offset, s.len));
        case Content::Kind::pending: {
            PendingBytes p = f.pending[s.content.page];
            if (p.inline_) return Body::inline_(std::span(f.scratch.data() + p.start, s.len));
            // Not placed yet: an estimate stands for its address.
            return Body::ref(s.len, 0);
        }
        case Content::Kind::free: break;
    }
    fail_check("an entry for a free span", __FILE__, __LINE__);
}

std::size_t Store::Inner::estimate_of(Flush const& f, std::uint32_t id) const {
    return estimate(id, state.span(id).next, body(f, id));
}

std::vector<std::uint32_t> Store::Inner::leaves() const {
    std::vector<std::uint32_t> out;
    for (auto const& [p, buf] : table_pages) {
        (void)buf;
        Page const& page = state.pages[p];
        if (page.is_live() && page.kind == PageKind::table) out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void Store::Inner::choose_header(Flush& f) {
    if (auto it = state.table_spans.find(header_slot); it != state.table_spans.end())
        for (std::uint32_t id : it->second) f.stated.insert(id);
    std::vector<std::uint32_t> cands(f.stated.begin(), f.stated.end());
    std::sort(cands.begin(), cands.end(), [&](std::uint32_t a, std::uint32_t b) {
        std::uint64_t ha = state.span(a).heat, hb = state.span(b).heat;
        return ha != hb ? ha > hb : a < b;
    });
    std::vector<std::size_t> ests;
    std::size_t total = 0;
    for (std::uint32_t id : cands) {
        ests.push_back(estimate_of(f, id));
        total += ests.back();
    }
    auto lv = leaves();
    std::size_t children = children_len(lv) + 3 * (total / MAX_PAGE_CONTENT + 1);
    std::size_t capacity = MAX_HEADER_CONTENT > children + 64 ? MAX_HEADER_CONTENT - (children + 64) : 0;
    std::size_t used = 0;
    for (std::size_t i = 0; i < cands.size(); ++i) {
        if (used + ests[i] > capacity) break;
        used += ests[i];
        f.header.insert(cands[i]);
    }
}

// ---------------------------------------------------------------- merges

bool Store::Inner::leafward(Flush const& f, std::uint32_t id) const {
    return state.span(id).chain == Chain::sequence && !f.header.contains(id);
}

expected<std::uint32_t> Store::Inner::merge_group(Flush& f, std::vector<std::uint32_t> const& group) {
    std::vector<std::uint32_t> ids;
    for (std::uint32_t n : group) ids.push_back(state.span_at(n));
    std::uint64_t pos = state.rope.pos_of(group.front());
    std::uint32_t pred = state.rope.prev(group.front());
    std::uint32_t succ = state.rope.next(group.back());
    std::uint32_t succ_id = succ == NIL ? 0 : state.span_at(succ);
    std::uint32_t pred_id = pred == NIL ? 0 : state.span_at(pred);
    auto start = static_cast<std::uint32_t>(f.scratch.size());
    std::uint64_t heat = 0;
    std::uint32_t total = 0;
    for (std::uint32_t id : ids) {
        KLADDE_TRY(span_to_scratch(f, id));
        heat = std::max(heat, state.span(id).heat);
        total += state.span(id).len;
    }
    for (std::uint32_t id : ids) kill_in(f, id);
    auto& rope = state.rope;
    auto [l, rest] = rope.split(state.root, pos);
    auto [mid, r] = rope.split(rest, total);
    rope.free_tree(mid);
    state.root = rope.merge(l, r);
    KLADDE_TRY_ASSIGN(auto made, new_spans(f, start, total, Chain::sequence, heat, pred_id));
    std::uint64_t at = pos;
    for (std::uint32_t id : made) {
        std::uint32_t slen = state.span(id).len;
        std::uint32_t node = rope.node(RopeItem::span(id, slen));
        state.span_mut(id).node = node;
        insert_at(at, node);
        at += slen;
    }
    state.span_mut(made.back()).next = succ_id;
    if (pred_id != 0) {
        state.span_mut(pred_id).next = made.front();
        f.stated.insert(pred_id);
    }
    return made.front();
}

expected<void> Store::Inner::merge_evicted(Flush& f) {
    std::uint32_t l = opts.span_min;
    std::vector<std::uint32_t> todo;
    for (std::uint32_t id : f.stated)
        if (leafward(f, id) && state.span(id).len < l) todo.push_back(id);
    std::sort(todo.begin(), todo.end());
    IdSet done;
    auto len_of = [&](std::uint32_t n) { return state.span(state.span_at(n)).len; };
    for (std::uint32_t id : todo) {
        if (done.contains(id) || state.span(id).chain != Chain::sequence) continue;
        std::uint32_t first = state.span(id).node;
        std::uint32_t last = first;
        for (;;) {
            std::uint32_t p = state.rope.prev(first);
            if (p == NIL || !leafward(f, state.span_at(p)) || len_of(p) + len_of(first) >= l) break;
            first = p;
        }
        for (;;) {
            std::uint32_t n = state.rope.next(last);
            if (n == NIL || !leafward(f, state.span_at(n)) || len_of(last) + len_of(n) >= l) break;
            last = n;
        }
        std::vector<std::uint32_t> run{first};
        for (std::uint32_t m = first; m != last;) {
            m = state.rope.next(m);
            run.push_back(m);
        }
        for (std::uint32_t n : run) done.insert(state.span_at(n));
        // Greedily, a group grows while it stays shorter than `l`.
        std::vector<std::vector<std::uint32_t>> groups;
        std::vector<std::uint32_t> cur;
        std::uint32_t cur_len = 0;
        for (std::uint32_t n : run) {
            std::uint32_t len_n = len_of(n);
            if (!cur.empty() && cur_len + len_n >= l) {
                groups.push_back(std::move(cur));
                cur.clear();
                cur_len = 0;
            }
            cur.push_back(n);
            cur_len += len_n;
        }
        groups.push_back(std::move(cur));
        for (auto const& g : groups) {
            if (g.size() < 2) continue;
            std::uint64_t bytes = 0;
            for (std::uint32_t n : g) bytes += len_of(n);
            KLADDE_TRY_ASSIGN(std::uint32_t made, merge_group(f, g));
            done.insert(made);
            stats.evict_merges += 1;
            stats.evict_merge_bytes += bytes;
        }
    }
    return {};
}

expected<void> Store::Inner::walk(Flush& f) {
    std::uint64_t e = state.flush_epoch;
    std::uint64_t cold_before = e > opts.cold_flushes ? e - opts.cold_flushes : 0;
    std::uint32_t id = cons.walk;
    if (id == 0 || id >= state.spans.size() || state.span(id).chain != Chain::sequence)
        id = state.seq_head();
    std::vector<std::uint32_t> run;
    std::size_t seen = 0;
    while (id != 0 && seen < opts.walk) {
        Span s = state.span(id);
        bool eligible = s.chain == Chain::sequence && s.heat <= cold_before && s.entry_page != NONE &&
                        s.entry_page != header_slot && !f.stated.contains(id);
        if (eligible) {
            run.push_back(id);
        } else {
            KLADDE_TRY(merge_run(f, run));
            run.clear();
        }
        id = s.next;
        seen += 1;
    }
    KLADDE_TRY(merge_run(f, run));
    cons.walk = id;
    return {};
}

expected<void> Store::Inner::merge_run(Flush& f, std::vector<std::uint32_t> const& run) {
    if (run.size() < 2) return {};
    double mu = opts.mu;
    auto weight = [&](std::uint32_t id) {
        Span const& s = state.span(id);
        double framing = double(s.entry_len) -
                         (s.content.kind == Content::Kind::inline_ ? double(s.len) : 0.0);
        return framing - mu * double(s.len);
    };
    // A merged span's own entry, about.
    constexpr double NEW_FRAMING = 6.0;
    std::vector<std::vector<std::uint32_t>> groups;
    std::vector<std::uint32_t> cur;
    double sum = 0.0;
    std::uint32_t cur_len = 0;
    auto close_group = [&]() {
        if (cur.size() >= 2 && sum > NEW_FRAMING) groups.push_back(cur);
        cur.clear();
    };
    for (std::uint32_t id : run) {
        double w = weight(id);
        std::uint32_t n = state.span(id).len;
        if (cur_len + n > C) {
            close_group();
            sum = 0.0;
            cur_len = 0;
        }
        sum += w;
        cur_len += n;
        cur.push_back(id);
        if (sum < 0.0) {
            cur.clear();
            sum = 0.0;
            cur_len = 0;
        }
    }
    close_group();
    for (auto const& g : groups) {
        std::vector<std::uint32_t> nodes;
        std::uint64_t bytes = 0;
        for (std::uint32_t id : g) {
            nodes.push_back(state.span(id).node);
            bytes += state.span(id).len;
        }
        KLADDE_TRY(merge_group(f, nodes));
        stats.walk_merges += 1;
        stats.walk_merge_bytes += bytes;
    }
    return {};
}

// ---------------------------------------------------------------- placement

expected<DataPage> Store::Inner::new_data_page() {
    KLADDE_TRY_ASSIGN(std::uint32_t page, take_page(Page::taken(PageKind::data)));
    return DataPage{page, new_page(), 0};
}

expected<void> Store::Inner::place(Flush& f, DataPage& dp, std::uint32_t id) {
    Span s = state.span(id);
    std::size_t n = s.len;
    std::size_t at = CONTENT_OFFSET + dp.used;
    switch (s.content.kind) {
        case Content::Kind::pending: {
            std::size_t p = f.pending[s.content.page].start;
            std::memcpy(dp.buf->data() + at, f.scratch.data() + p, n);
            break;
        }
        case Content::Kind::ref: {
            KLADDE_TRY_ASSIGN(PageBuf const* src, page_bytes(s.content.page));
            std::memcpy(dp.buf->data() + at, src->data() + s.content.offset, n);
            state.unnote_data(id);
            state.uncover(s.content.page, s.len);
            break;
        }
        default: return corrupt("placing span " + std::to_string(id) + ", which shows no page's bytes");
    }
    state.span_mut(id).content = Content::ref(dp.page, static_cast<std::uint16_t>(at));
    state.note_data(id);
    state.cover(dp.page, s.len);
    f.stated.insert(id);
    dp.used += n;
    return {};
}

expected<std::uint32_t> Store::Inner::split_span(Flush& f, std::uint32_t id, std::uint32_t head) {
    Span s = state.span(id);
    KLADDE_DEBUG_CHECK(head > 0 && head < s.len, "splitting a span outside it");
    Content content;
    if (s.content.kind == Content::Kind::pending) {
        PendingBytes p = f.pending[s.content.page];
        f.pending.push_back({p.start + head, false});
        content = Content::pending(static_cast<std::uint32_t>(f.pending.size() - 1));
    } else if (s.content.kind == Content::Kind::ref) {
        content = Content::ref(s.content.page, static_cast<std::uint16_t>(s.content.offset + head));
    } else {
        return corrupt("splitting span " + std::to_string(id) + ", which shows no page's bytes");
    }
    auto t = state.new_span(id);
    if (!t) return errc::out_of_bounds;
    Span& ts = state.span_mut(*t);
    ts = Span{};
    ts.len = s.len - head;
    ts.next = s.next;
    ts.content = content;
    ts.chain = s.chain;
    ts.entry_page = NONE;
    ts.heat = s.heat;
    ts.node = NIL;
    state.span_mut(id).len = head;
    state.span_mut(id).next = *t;
    state.note_data(*t);
    switch (s.chain) {
        case Chain::sequence: {
            std::uint64_t pos = state.rope.pos_of(s.node);
            state.rope.set_item(s.node, RopeItem::span(id, head));
            std::uint32_t m = state.rope.node(RopeItem::span(*t, s.len - head));
            state.span_mut(*t).node = m;
            insert_at(pos + head, m);
            break;
        }
        case Chain::schema:
        case Chain::private_: {
            auto& list = s.chain == Chain::schema ? state.schema : state.private_;
            auto it = std::find(list.begin(), list.end(), id);
            KLADDE_CHECK(it != list.end(), "a span is missing from its chain");
            list.insert(it + 1, *t);
            break;
        }
        case Chain::none: KLADDE_CHECK(false, "splitting a free span");
    }
    f.stated.insert(id);
    f.stated.insert(*t);
    if (f.header.contains(id)) f.header.insert(*t);
    return *t;
}

std::vector<std::uint32_t> Store::Inner::chunks(Flush const& f) const {
    std::vector<std::uint32_t> small;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> seq;
    for (std::uint32_t id : f.stated) {
        Span const& s = state.span(id);
        if (s.content.kind != Content::Kind::pending || f.pending[s.content.page].inline_) continue;
        if (s.chain == Chain::sequence)
            seq.emplace_back(state.rope.pos_of(s.node), id);
        else
            small.push_back(id);
    }
    std::sort(small.begin(), small.end());
    std::sort(seq.begin(), seq.end());
    for (auto const& [pos, id] : seq) {
        (void)pos;
        small.push_back(id);
    }
    return small;
}

expected<void> Store::Inner::pack(Flush& f, Output& out) {
    auto c = chunks(f);
    std::deque<std::uint32_t> rem(c.begin(), c.end());
    if (rem.empty()) return {};
    auto theta = static_cast<std::size_t>(opts.theta * double(C));
    std::uint32_t inline_max = std::min<std::uint32_t>(opts.inline_threshold, MAX_INLINE);
    KLADDE_TRY_ASSIGN(DataPage open, new_data_page());
    for (;;) {
        auto room = static_cast<std::uint32_t>(open.room());
        std::size_t look = std::min(rem.size(), opts.lookahead);
        std::optional<std::size_t> fit;
        for (std::size_t i = 0; i < look; ++i) {
            if (state.span(rem[i]).len <= room) {
                fit = i;
                break;
            }
        }
        if (fit) {
            std::uint32_t id = rem[*fit];
            rem.erase(rem.begin() + static_cast<std::ptrdiff_t>(*fit));
            KLADDE_TRY(place(f, open, id));
            continue;
        }
        if (opts.consolidate) KLADDE_TRY(free_fill(f, open));
        if (rem.empty()) break;
        if (open.room() > theta) {
            std::uint32_t id = rem.front();
            rem.pop_front();
            auto head = static_cast<std::uint32_t>(open.room());
            KLADDE_TRY_ASSIGN(std::uint32_t t, split_span(f, id, head));
            KLADDE_TRY(place(f, open, id));
            Span const& ts = state.span(t);
            if (ts.len <= inline_max) {
                if (ts.content.kind == Content::Kind::pending) f.pending[ts.content.page].inline_ = true;
            } else {
                rem.push_front(t);
            }
        }
        if (rem.empty()) break;
        KLADDE_TRY_ASSIGN(DataPage next, new_data_page());
        out.data.push_back(std::exchange(open, std::move(next)));
    }
    out.data.push_back(std::move(open));
    return {};
}

// ---------------------------------------------------------------- the cut

std::vector<std::pair<std::vector<std::uint8_t>, std::vector<Placed>>> Store::Inner::encode_leaves(
    Flush const& f, std::vector<std::uint32_t> const& ids) const {
    std::vector<std::pair<std::vector<std::uint8_t>, std::vector<Placed>>> pages;
    PageEncoder enc({}, MAX_PAGE_CONTENT);
    for (std::uint32_t id : ids) {
        EntryOut e{id, state.span(id).next, body(f, id)};
        if (!enc.push(e)) {
            pages.emplace_back(std::move(enc.out), std::move(enc.placed));
            enc = PageEncoder({}, MAX_PAGE_CONTENT);
            KLADDE_CHECK(enc.push(e), "an entry larger than a page");
        }
    }
    if (!enc.placed.empty()) pages.emplace_back(std::move(enc.out), std::move(enc.placed));
    return pages;
}

std::optional<std::pair<std::vector<std::uint8_t>, std::vector<Placed>>> Store::Inner::encode_header(
    Flush const& f, std::vector<std::uint32_t> const& children,
    std::vector<std::uint32_t> const& ids) const {
    PageEncoder enc(children, MAX_HEADER_CONTENT);
    for (std::uint32_t id : ids) {
        if (!enc.push(EntryOut{id, state.span(id).next, body(f, id)})) return std::nullopt;
    }
    return std::pair{std::move(enc.out), std::move(enc.placed)};
}

expected<void> Store::Inner::cut(Flush& f, Output& out) {
    std::vector<std::uint32_t> stated = sorted(f.stated);
    for (std::uint32_t id : stated) state.drop_entry(id);
    std::vector<std::uint32_t> head, leaf;
    for (std::uint32_t id : stated) (f.header.contains(id) ? head : leaf).push_back(id);
    for (;;) {
        auto pages = encode_leaves(f, leaf);
        // Fillers: whole leaves whose entries fit the last leaf's room.
        if (opts.consolidate && !pages.empty()) {
            std::size_t room = MAX_PAGE_CONTENT - pages.back().first.size();
            auto victims = cut_fillers(f, room);
            if (!victims.empty()) {
                for (std::uint32_t v : victims) {
                    auto it = state.table_spans.find(v);
                    std::vector<std::uint32_t> ids =
                        it == state.table_spans.end() ? std::vector<std::uint32_t>{} : it->second;
                    for (std::uint32_t id : ids) {
                        state.drop_entry(id);
                        f.stated.insert(id);
                        leaf.push_back(id);
                    }
                    stats.table_rewrites += 1;
                }
                std::sort(leaf.begin(), leaf.end());
                pages = encode_leaves(f, leaf);
            }
        }
        std::vector<std::uint32_t> taken;
        for (std::size_t i = 0; i < pages.size(); ++i) {
            KLADDE_TRY_ASSIGN(std::uint32_t p, take_page(Page::taken(PageKind::table)));
            taken.push_back(p);
        }
        std::vector<std::uint32_t> children;
        for (std::uint32_t p : leaves())
            if (state.coverage(p) > 0) children.push_back(p);
        children.insert(children.end(), taken.begin(), taken.end());
        std::sort(children.begin(), children.end());
        auto encoded = encode_header(f, children, head);
        if (encoded) {
            // Record where every entry landed.
            for (std::size_t i = 0; i < pages.size(); ++i) {
                std::uint32_t page = taken[i];
                auto buf = new_page();
                encode_page(*buf, nullptr, KIND_TABLE, state.flush_epoch, pages[i].first);
                auto copy = std::make_unique<PageBuf>(*buf);
                table_pages[page] = std::move(buf);
                note_placed(f, page, CONTENT_OFFSET, pages[i].second);
                out.tables.emplace_back(page, std::move(copy));
            }
            auto slot = static_cast<std::uint32_t>(state.flush_epoch % 2);
            note_placed(f, slot, HEADER_CONTENT_OFFSET, encoded->second);
            out.header_content = std::move(encoded->first);
            stats.entries_stated += head.size() + leaf.size();
            return {};
        }
        for (std::uint32_t p : taken) {
            state.pages[p] = Page::reusable();
            ready.insert(p);
        }
        if (head.empty()) {
            // The references to leaves alone outgrow the header, which only
            // interior table pages would relieve, and this implementation
            // writes none.
            return errc::out_of_bounds;
        }
        // The coldest eighth of the header's entries go to leaves.
        std::sort(head.begin(), head.end(), [&](std::uint32_t a, std::uint32_t b) {
            std::uint64_t ha = state.span(a).heat, hb = state.span(b).heat;
            return ha != hb ? ha > hb : a < b;
        });
        std::size_t keep = head.size() - (head.size() + 7) / 8;
        for (std::size_t i = keep; i < head.size(); ++i) {
            f.header.erase(head[i]);
            leaf.push_back(head[i]);
        }
        head.resize(keep);
        std::sort(head.begin(), head.end());
        std::sort(leaf.begin(), leaf.end());
    }
}

void Store::Inner::note_placed(Flush const& f, std::uint32_t page, std::size_t content_at,
                               std::vector<Placed> const& placed) {
    if (page < 2 && !state.pages[page].is(Page::State::filling)) {
        // The header slot the flush writes is taken here, its old content
        // having been moved.
        state.pages[page] = Page::taken(PageKind::header);
    }
    for (Placed const& p : placed) {
        Span const& s = state.span(p.id);
        bool is_inline = s.content.kind == Content::Kind::inline_ ||
                         (s.content.kind == Content::Kind::pending && f.pending[s.content.page].inline_);
        if (is_inline)
            state.span_mut(p.id).content =
                Content::inline_at(page, static_cast<std::uint16_t>(content_at + p.payload));
        state.note_entry(p.id, page, p.len);
    }
}

// ---------------------------------------------------------------- commit

expected<void> Store::Inner::commit(std::uint64_t e, Output out) {
    // The next journal starts where the seal said, or else in the lowest
    // reusable page, or past the end.
    std::optional<std::uint32_t> sealed = std::exchange(sealed_next, std::nullopt);
    std::uint32_t jp;
    if (sealed) {
        jp = *sealed;
    } else if (!ready.empty()) {
        jp = *ready.begin();
        ready.erase(ready.begin());
    } else {
        jp = file_pages;
    }
    auto slot = static_cast<std::uint32_t>(e % 2);
    if (!state.pages[slot].is(Page::State::filling)) state.pages[slot] = Page::taken(PageKind::header);
    std::vector<std::uint32_t> written;
    for (auto const& dp : out.data) written.push_back(dp.page);
    for (auto const& [p, buf] : out.tables) {
        (void)buf;
        written.push_back(p);
    }
    written.push_back(slot);
    KLADDE_DEBUG_CHECK(written.size() == static_cast<std::size_t>(std::count_if(
                                             state.pages.begin(), state.pages.end(),
                                             [](Page const& p) { return p.is(Page::State::filling); })),
                       "a page the flush took is not written");
    for (auto& dp : out.data) {
        seal_page(*dp.buf, nullptr, KIND_DATA, e, dp.used);
        KLADDE_TRY(storage->write_at(*dp.buf, std::uint64_t{dp.page} * PAGE_SIZE));
        stats.data_pages_written += 1;
    }
    for (auto const& [p, buf] : out.tables) {
        KLADDE_TRY(storage->write_at(*buf, std::uint64_t{p} * PAGE_SIZE));
        stats.table_pages_written += 1;
    }
    // A session's first flush clears the page it names for the next journal
    // (spec/journal.md#the-start-of-a-session), unless the seal did already.
    if (first_flush && !sealed && jp < file_pages)
        KLADDE_TRY(storage->write_at(*new_page(), std::uint64_t{jp} * PAGE_SIZE));
    header.journal_pointer = jp;
    header.format_version = FORMAT_VERSION;
    header.min_reader_version = MIN_READER_VERSION;
    header.sequence_head = state.seq_head();
    header.schema_head = state.schema.empty() ? 0 : state.schema.front();
    header.private_head = state.private_.empty() ? 0 : state.private_.front();
    auto hbuf = new_page();
    encode_page(*hbuf, &header, KIND_TABLE, e, out.header_content);
    // The `fsync` and the header write, on a thread of their own when a
    // mutation triggered the flush and the journal is sealed: the next
    // journal is reachable through the seal whichever header governs, so
    // appends may go on meanwhile.
    std::uint64_t at = std::uint64_t{slot} * PAGE_SIZE;
    std::optional<PendingCommit> worker;
    std::unique_ptr<Storage> second;
    if (sealed && opts.sync_on_worker && !explicit_flush) second = storage->try_clone();
    if (second) {
        stats.commits_on_worker += 1;
        auto result = std::make_unique<expected<void>>();
        auto page = std::make_unique<PageBuf>(*hbuf);
        std::thread t([s = std::move(second), page = std::move(page), at, r = result.get()] {
            auto synced = s->sync();
            *r = synced ? s->write_at(*page, at) : synced;
        });
        worker = PendingCommit{std::move(t), std::move(result), {}};
    } else {
        KLADDE_TRY(storage->sync());
        KLADDE_TRY(storage->write_at(*hbuf, at));
    }
    stats.headers_written += 1;

    // The pages the flush filled are in the world now.
    for (std::uint32_t p : written) {
        Page& page = state.pages[p];
        KLADDE_CHECK(page.is(Page::State::filling), "a page was written but is not filling");
        page = Page::live(e, page.coverage, page.kind);
    }

    // Bookkeeping: the rotation of reusable pages first. The pages the commit
    // before freed are reusable once this commit's `fsync` is done, which a
    // worker reports when it is joined.
    std::vector<std::uint32_t> release = std::exchange(retiring, {});
    if (worker) {
        worker->release = std::move(release);
        pending = std::move(worker);
    } else {
        for (std::uint32_t p : release) {
            KLADDE_DEBUG_CHECK(state.pages[p].is(Page::State::fallback), "a retiring page is not fallback");
            state.pages[p] = Page::reusable();
            ready.insert(p);
        }
    }
    std::vector<std::uint32_t> next_retiring;
    std::vector<std::uint32_t> candidates = sorted(state.dropped_candidates);
    state.dropped_candidates.clear();
    for (std::uint32_t p : candidates) {
        if (p == slot || p == header_slot) continue;
        Page const& page = state.pages[p];
        if (page.is_live() && page.coverage == 0) {
            KLADDE_DEBUG_CHECK(page.kind != PageKind::header, "a header among the dropped pages");
            state.unbucket(p);
            state.pages[p] = Page::fallback();
            table_pages.erase(p);
            mirror.erase(p);
            cache.erase(p);
            next_retiring.push_back(p);
        }
    }
    for (std::uint32_t p : journal.pages) {
        if (p < file_pages && state.pages[p].is(Page::State::journal)) {
            state.pages[p] = Page::fallback();
            next_retiring.push_back(p);
        }
    }
    // The header this commit replaced leaves the world; the flush moved
    // everything it held. A header slot is never handed out, so it does not
    // join the rotation of reusable pages.
    std::uint32_t old = header_slot;
    if (old != slot) {
        KLADDE_DEBUG_CHECK(!state.table_spans.contains(old),
                           "the replaced header still holds live entries");
        state.pages[old] = Page::fallback();
    }
    retiring = std::move(next_retiring);
    table_pages[slot] = std::move(hbuf);
    if (old != slot) table_pages.erase(old);
    header_slot = slot;
    epoch = e;
    for (std::uint32_t p : written) state.rebucket(p);
    if (jp < file_pages) {
        KLADDE_DEBUG_CHECK(state.pages[jp].is(Page::State::reusable) ||
                               (sealed && state.pages[jp].is(Page::State::journal)),
                           "the next journal's page is in use");
        state.pages[jp] = Page::journal();
    }
    journal = JournalWriter(e + 1, jp);
    first_flush = false;
    cache.clear();
    stats.flushes += 1;
    after_commit();
    return truncate(false);
}

expected<void> Store::Inner::truncate(bool force) {
    std::uint32_t top = file_pages;
    while (top > 2) {
        std::uint32_t p = top - 1;
        Page const& page = state.pages[p];
        bool empty_journal = page.is(Page::State::journal) && journal.is_empty() &&
                             journal.pages.size() == 1 && journal.pages[0] == p;
        if (page.is(Page::State::reusable) || (force && empty_journal))
            top -= 1;
        else
            break;
    }
    std::uint32_t tail = file_pages - top;
    if (tail == 0 || (!force && tail < opts.truncate_tail)) return {};
    KLADDE_TRY(storage->resize(std::uint64_t{top} * PAGE_SIZE));
    for (std::uint32_t p = top; p < file_pages; ++p) ready.erase(p);
    state.pages.resize(std::max<std::uint32_t>(top, 2));
    file_pages = top;
    stats.truncations += 1;
    return {};
}

}  // namespace kladde

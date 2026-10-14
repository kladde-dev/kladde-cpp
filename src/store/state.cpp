#include "state.hpp"

#include <algorithm>
#include <bit>
#include <string>

namespace kladde::store {

// ---------------------------------------------------------------- Ids

Ids::Ids() {
    // Id 0 is never a span.
    mark(0);
    count_ = 0;
}

bool Ids::is_used(std::uint32_t id) const {
    std::size_t w = id / 64;
    return w < used_.size() && (used_[w] & (std::uint64_t{1} << (id % 64))) != 0;
}

void Ids::mark(std::uint32_t id) {
    std::size_t w = id / 64;
    if (w >= used_.size()) {
        used_.resize(w + 1, 0);
        full_.resize((used_.size() + 63) / 64, 0);
    }
    KLADDE_DEBUG_CHECK((used_[w] & (std::uint64_t{1} << (id % 64))) == 0, "span id in use");
    used_[w] |= std::uint64_t{1} << (id % 64);
    if (used_[w] == UINT64_MAX) full_[w / 64] |= std::uint64_t{1} << (w % 64);
    count_ += 1;
}

void Ids::free(std::uint32_t id) {
    std::size_t w = id / 64;
    KLADDE_DEBUG_CHECK(is_used(id), "freeing a span id that is free");
    used_[w] &= ~(std::uint64_t{1} << (id % 64));
    full_[w / 64] &= ~(std::uint64_t{1} << (w % 64));
    count_ -= 1;
}

std::optional<std::uint32_t> Ids::alloc(std::uint32_t after) {
    if (after != 0 && after < UINT32_MAX && !is_used(after + 1)) {
        mark(after + 1);
        return after + 1;
    }
    for (std::size_t i = 0; i < full_.size(); ++i) {
        std::uint64_t f = full_[i];
        if (f != UINT64_MAX) {
            std::size_t w = i * 64 + static_cast<std::size_t>(std::countr_zero(~f));
            if (w < used_.size()) {
                auto id = static_cast<std::uint32_t>(w * 64 + static_cast<std::size_t>(
                                                                  std::countr_zero(~used_[w])));
                mark(id);
                return id;
            }
            break;
        }
    }
    std::uint64_t id = used_.size() * 64;
    if (id >= UINT32_MAX) return std::nullopt;
    mark(static_cast<std::uint32_t>(id));
    return static_cast<std::uint32_t>(id);
}

// ---------------------------------------------------------------- Buckets

std::uint8_t Buckets::bucket_of(std::uint32_t coverage) {
    std::size_t b = (std::size_t{coverage} * BUCKETS) / (MAX_PAGE_CONTENT + 1);
    return static_cast<std::uint8_t>(std::min(b, BUCKETS - 1));
}

std::optional<std::size_t> Buckets::lowest_non_empty() const {
    for (std::size_t i = 0; i < BUCKETS; ++i)
        if (!lists[i].empty()) return i;
    return std::nullopt;
}

// ---------------------------------------------------------------- pages

void State::ensure_page(std::uint32_t page) {
    if (pages.size() <= page) pages.resize(std::size_t{page} + 1, Page::reusable());
}

Page const& State::live(std::uint32_t page) const {
    KLADDE_CHECK(pages[page].is_live(), "a page expected to be live is not");
    return pages[page];
}

Page& State::live_mut(std::uint32_t page) {
    KLADDE_CHECK(pages[page].is_live(), "a page expected to be live is not");
    return pages[page];
}

std::uint32_t& State::coverage_mut(std::uint32_t page) {
    Page& p = pages[page];
    KLADDE_CHECK(p.is_live() || p.is(Page::State::filling), "a page that holds no content is covered");
    return p.coverage;
}

std::uint32_t State::coverage(std::uint32_t page) const {
    Page const& p = pages[page];
    return p.is_live() || p.is(Page::State::filling) ? p.coverage : 0;
}

Page const* State::victim(std::uint32_t page) const {
    Page const& p = pages[page];
    if (p.is_live() && p.coverage > 0 && p.kind != PageKind::header) return &p;
    return nullptr;
}

std::optional<std::uint8_t> State::wanted_bucket(std::uint32_t page) const {
    if (Page const* p = victim(page)) return Buckets::bucket_of(p->coverage);
    return std::nullopt;
}

std::optional<std::uint8_t> State::current_bucket(std::uint32_t page) const {
    Page const& p = pages[page];
    if (p.is_live() && p.slot) return p.slot->bucket;
    return std::nullopt;
}

Buckets& State::buckets_of(PageKind kind) {
    KLADDE_CHECK(kind != PageKind::header, "a header is never a victim");
    return kind == PageKind::data ? data_buckets : table_buckets;
}

void State::rebucket(std::uint32_t page) {
    auto want = wanted_bucket(page);
    if (current_bucket(page) == want) return;
    unbucket(page);
    if (want) {
        auto& list = buckets_of(live(page).kind).lists[*want];
        auto index = static_cast<std::uint32_t>(list.size());
        list.push_back(page);
        live_mut(page).slot = BucketSlot{*want, index};
    }
}

void State::unbucket(std::uint32_t page) {
    Page const& p = pages[page];
    if (!p.is_live() || !p.slot) return;
    auto& list = buckets_of(p.kind).lists[p.slot->bucket];
    std::size_t index = p.slot->index;
    // Swap-remove, as the bucket's order does not matter.
    list[index] = list.back();
    list.pop_back();
    if (index < list.size()) {
        auto& moved = live_mut(list[index]).slot;
        if (moved) moved->index = static_cast<std::uint32_t>(index);
    }
    live_mut(page).slot.reset();
}

void State::cover(std::uint32_t page, std::uint32_t n) {
    if (n == 0) return;
    coverage_mut(page) += n;
    if (wanted_bucket(page) != current_bucket(page)) rebucket(page);
}

void State::uncover(std::uint32_t page, std::uint32_t n) {
    if (n == 0) return;
    std::uint32_t& coverage = coverage_mut(page);
    KLADDE_DEBUG_CHECK(coverage >= n, "the coverage of a page underflows");
    coverage -= n;
    dropped_candidates.insert(page);
    if (wanted_bucket(page) != current_bucket(page)) rebucket(page);
}

// ---------------------------------------------------------------- spans

std::optional<std::uint32_t> State::new_span(std::uint32_t after) {
    auto id = ids.alloc(after);
    if (!id) return std::nullopt;
    if (spans.size() <= *id) spans.resize(std::size_t{*id} + 1);
    spans[*id] = Span{};
    spans[*id].entry_page = NONE;
    spans[*id].node = NIL;
    return id;
}

std::uint32_t State::span_at(std::uint32_t node) const {
    RopeItem const& it = rope.item(node);
    KLADDE_CHECK(!it.lit, "a literal where a span was expected");
    return it.id_or_start;
}

std::uint32_t State::seq_head() const {
    std::uint32_t n = rope.first(root);
    return n == NIL ? 0 : span_at(n);
}

void State::note_data(std::uint32_t id) {
    Content c = spans[id].content;
    if (c.kind != Content::Kind::ref) return;
    auto& list = data_spans[c.page];
    spans[id].rev = static_cast<std::uint32_t>(list.size());
    list.push_back(id);
}

void State::unnote_data(std::uint32_t id) {
    Content c = spans[id].content;
    if (c.kind != Content::Kind::ref) return;
    std::size_t rev = spans[id].rev;
    auto it = data_spans.find(c.page);
    KLADDE_CHECK(it != data_spans.end(), "a data page's list is missing");
    auto& list = it->second;
    KLADDE_DEBUG_CHECK(list[rev] == id, "a span is not where its data page's list has it");
    list[rev] = list.back();
    list.pop_back();
    if (rev < list.size()) spans[list[rev]].rev = static_cast<std::uint32_t>(rev);
    if (list.empty()) data_spans.erase(it);
}

void State::note_entry(std::uint32_t id, std::uint32_t page, std::uint16_t len) {
    KLADDE_DEBUG_CHECK(spans[id].entry_page == NONE, "a span gets a second live entry");
    auto& list = table_spans[page];
    Span& s = spans[id];
    s.tab = static_cast<std::uint32_t>(list.size());
    s.entry_page = page;
    s.entry_len = len;
    list.push_back(id);
    cover(page, len);
}

void State::drop_entry(std::uint32_t id) {
    Span s = spans[id];
    if (s.entry_page == NONE) return;
    auto it = table_spans.find(s.entry_page);
    KLADDE_CHECK(it != table_spans.end(), "a table page's list is missing");
    auto& list = it->second;
    std::size_t tab = s.tab;
    KLADDE_DEBUG_CHECK(list[tab] == id, "a span is not where its table page's list has it");
    list[tab] = list.back();
    list.pop_back();
    if (tab < list.size()) spans[list[tab]].tab = static_cast<std::uint32_t>(tab);
    if (list.empty()) table_spans.erase(it);
    uncover(s.entry_page, s.entry_len);
    spans[id].entry_page = NONE;
}

void State::kill(std::uint32_t id) {
    drop_entry(id);
    Span s = spans[id];
    if (s.content.kind == Content::Kind::ref) {
        unnote_data(id);
        uncover(s.content.page, s.len);
    }
    spans[id] = Span{};
    ids.free(id);
}

std::vector<std::uint32_t> State::seq_spans() const {
    std::vector<std::uint32_t> out;
    for (std::uint32_t n : rope.nodes(root)) out.push_back(span_at(n));
    return out;
}

void State::check(std::uint32_t header_slot) const {
    rope.check(root);
    IdMap<std::uint64_t> data_cov;
    IdMap<std::uint64_t> table_cov;
    IdSet reached;
    std::pair<Chain, std::vector<std::uint32_t>> chains[] = {
        {Chain::sequence, seq_spans()}, {Chain::schema, schema}, {Chain::private_, private_}};
    for (auto const& [chain, list] : chains) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            std::uint32_t id = list[i];
            KLADDE_CHECK(reached.insert(id).second, "a span is reached twice");
            Span const& s = span(id);
            KLADDE_CHECK(s.chain == chain, "a span is in another chain");
            KLADDE_CHECK(s.len > 0, "a span is empty");
            std::uint32_t next = i + 1 < list.size() ? list[i + 1] : 0;
            KLADDE_CHECK(s.next == next, "a span links to the wrong span");
            if (chain == Chain::sequence)
                KLADDE_CHECK(rope.item(s.node) == RopeItem::span(id, s.len), "a span's rope node");
            KLADDE_CHECK(s.entry_page != NONE, "a span has no entry");
            table_cov[s.entry_page] += s.entry_len;
            auto tl = table_spans.find(s.entry_page);
            KLADDE_CHECK(tl != table_spans.end() && tl->second.size() > s.tab &&
                             tl->second[s.tab] == id,
                         "a span is not in its table page's list");
            switch (s.content.kind) {
                case Content::Kind::ref: {
                    KLADDE_CHECK(std::size_t{s.content.offset} + s.len <= PAGE_SIZE - 4,
                                 "a span reaches past its page");
                    data_cov[s.content.page] += s.len;
                    auto dl = data_spans.find(s.content.page);
                    KLADDE_CHECK(dl != data_spans.end() && dl->second.size() > s.rev &&
                                     dl->second[s.rev] == id,
                                 "a span is not in its data page's list");
                    break;
                }
                case Content::Kind::inline_:
                    KLADDE_CHECK(s.content.page == s.entry_page, "a span's payload is elsewhere");
                    KLADDE_CHECK(s.len <= MAX_INLINE, "an inline span is too long");
                    break;
                case Content::Kind::zero: break;
                default: KLADDE_CHECK(false, "a span shows a pending or free content between flushes");
            }
        }
    }
    KLADDE_CHECK(reached.size() == ids.count(), "spans nobody reaches");
    for (auto const& [page, list] : data_spans) {
        (void)page;
        for (std::uint32_t id : list) KLADDE_CHECK(reached.contains(id), "a data page lists a dead span");
    }
    for (std::size_t p = 0; p < pages.size(); ++p) {
        auto pg = static_cast<std::uint32_t>(p);
        Page const& page = pages[p];
        if (page.is_live()) {
            switch (page.kind) {
                case PageKind::data: {
                    auto it = data_cov.find(pg);
                    KLADDE_CHECK(page.coverage == (it == data_cov.end() ? 0 : it->second),
                                 "the coverage of a data page");
                    break;
                }
                case PageKind::table: {
                    auto it = table_cov.find(pg);
                    KLADDE_CHECK(page.coverage == (it == table_cov.end() ? 0 : it->second),
                                 "the coverage of a leaf");
                    break;
                }
                case PageKind::header: KLADDE_CHECK(pg == header_slot, "a stale header"); break;
            }
            std::optional<std::uint8_t> slot_bucket;
            if (page.slot) slot_bucket = page.slot->bucket;
            KLADDE_CHECK(slot_bucket == wanted_bucket(pg), "a page sits in the wrong bucket");
        } else {
            KLADDE_CHECK(!data_cov.contains(pg) && !table_cov.contains(pg),
                         "a page that is not live holds live content");
        }
    }
}

}  // namespace kladde::store

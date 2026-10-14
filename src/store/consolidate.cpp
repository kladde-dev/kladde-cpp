// Consolidation: evacuating sparse data pages, in the room of pages the flush
// writes anyway and on pages of their own; draining sparse leaves; compaction
// mode; and the controller that paces it.
//
// The policy is the churn floor: a budgeted victim must free `churn_floor`
// bytes of space per byte it writes. Its survivors are whole spans, which
// keep their ids and are stated with their new addresses.
#include <algorithm>
#include <cmath>

#include "inner.hpp"
#include "util.hpp"

namespace kladde {

using namespace store;

namespace {

/// A page's content capacity.
constexpr std::uint32_t C = MAX_PAGE_CONTENT;

}  // namespace

void Store::Inner::begin_consolidation() {
    cons.skip.clear();
    cons.tail = file_pages > 0 ? file_pages - 1 : 0;
    std::size_t holes = 0;
    if (auto t = tail()) {
        for (std::uint32_t p = 2; p < *t; ++p)
            if (state.pages[p].is(Page::State::reusable)) holes += 1;
    }
    cons.compaction = opts.consolidate && double(holes) > opts.hole_share * double(file_pages);
    if (cons.compaction) stats.compaction_flushes += 1;
}

std::optional<std::uint32_t> Store::Inner::tail() {
    std::uint32_t p = cons.tail;
    while (p >= 2 && p < state.pages.size()) {
        if (state.victim(p) && !cons.skip.contains(p)) {
            cons.tail = p;
            return p;
        }
        p -= 1;
    }
    cons.tail = std::min(p, cons.tail);
    return std::nullopt;
}

std::optional<std::uint32_t> Store::Inner::compaction_tail(bool data) {
    if (!cons.compaction) return std::nullopt;
    auto t = tail();
    if (t && (state.live(*t).kind == PageKind::data) == data) return t;
    return std::nullopt;
}

std::optional<std::uint32_t> Store::Inner::compaction_victim(bool data) {
    std::optional<std::uint32_t> lowest;
    if (!ready.empty()) lowest = *ready.begin();
    auto t = compaction_tail(data);
    if (t && lowest && *lowest < *t) return t;
    return std::nullopt;
}

// ---------------------------------------------------------------- victims

double Store::Inner::score(std::uint32_t p) const {
    Page const& live = state.live(p);
    double u = double(live.coverage) / double(C);
    double age = state.flush_epoch > live.epoch ? double(state.flush_epoch - live.epoch) : 0.0;
    return (1.0 - u) * age / (1.0 + u);
}

std::vector<std::uint32_t> Store::Inner::sample(bool data, std::size_t bucket) {
    std::size_t k = std::max<std::size_t>(opts.sample, 1);
    auto const& list = (data ? state.data_buckets : state.table_buckets).lists[bucket];
    std::size_t n = list.size();
    if (n == 0) return {};
    std::size_t start = cons.rot % n;
    cons.rot += 1;
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < std::min(k, n); ++i) out.push_back(list[(start + i) % n]);
    return out;
}

std::optional<std::uint32_t> Store::Inner::pick_victim(bool data, std::size_t from,
                                                       std::uint32_t max, VictimFilter const& ok) {
    for (std::size_t b = from; b < BUCKETS; ++b) {
        auto lower = static_cast<std::uint32_t>((b * (std::uint64_t{C} + 1) + BUCKETS - 1) / BUCKETS);
        if (lower > max) return std::nullopt;
        std::optional<std::uint32_t> best;
        double best_score = 0;
        for (std::uint32_t p : sample(data, b)) {
            std::uint32_t cov = state.live(p).coverage;
            if (cov == 0 || cov > max || cons.skip.contains(p) || !ok(*this, p)) continue;
            // The last of equally good candidates, as Rust's `max_by` takes.
            double s = score(p);
            if (!best || s >= best_score) {
                best = p;
                best_score = s;
            }
        }
        if (best) return best;
    }
    return std::nullopt;
}

std::vector<std::uint32_t> Store::Inner::survivors(std::uint32_t v) const {
    std::vector<std::uint32_t> out;
    if (auto it = state.data_spans.find(v); it != state.data_spans.end()) out = it->second;
    std::stable_sort(out.begin(), out.end(), [&](std::uint32_t a, std::uint32_t b) {
        Content ca = state.span(a).content, cb = state.span(b).content;
        std::uint16_t oa = ca.kind == Content::Kind::ref ? ca.offset : 0;
        std::uint16_t ob = cb.kind == Content::Kind::ref ? cb.offset : 0;
        return oa < ob;
    });
    return out;
}

expected<bool> Store::Inner::evacuate_into(Flush& f, std::uint32_t v, DataPage& dp) {
    cons.skip.insert(v);
    std::uint32_t total = state.live(v).coverage;
    if (total > dp.room()) return false;
    for (std::uint32_t id : survivors(v)) KLADDE_TRY(place(f, dp, id));
    stats.evacuated_pages += 1;
    stats.evacuated_bytes += total;
    return true;
}

expected<std::vector<std::uint32_t>> Store::Inner::evacuate_across(Flush& f, std::uint32_t v,
                                                                   DataPage& dp) {
    cons.skip.insert(v);
    std::uint32_t total = state.live(v).coverage;
    std::vector<std::uint32_t> rest;
    for (std::uint32_t id : survivors(v)) {
        auto room = static_cast<std::uint32_t>(dp.room());
        std::uint32_t span_len = state.span(id).len;
        if (!rest.empty() || room == 0) {
            rest.push_back(id);
        } else if (span_len <= room) {
            KLADDE_TRY(place(f, dp, id));
        } else if (state.span(id).chain == Chain::sequence) {
            KLADDE_TRY_ASSIGN(std::uint32_t t, split_span(f, id, room));
            KLADDE_TRY(place(f, dp, id));
            rest.push_back(t);
        } else {
            rest.push_back(id);
        }
    }
    stats.evacuated_pages += 1;
    stats.evacuated_bytes += total;
    return rest;
}

// ---------------------------------------------------------------- free filling

expected<void> Store::Inner::free_fill(Flush& f, DataPage& open) {
    auto any = [](Inner const&, std::uint32_t) { return true; };
    while (auto b = state.data_buckets.lowest_non_empty()) {
        auto room = static_cast<std::uint32_t>(open.room());
        if (room == 0) return {};
        std::optional<std::uint32_t> v = compaction_tail(true);
        if (v && !(open.page < *v && state.live(*v).coverage <= room)) v.reset();
        if (!v) v = pick_victim(true, *b, room, any);
        if (!v) break;
        KLADDE_TRY_ASSIGN(bool moved, evacuate_into(f, *v, open));
        if (moved) stats.free_filled_pages += 1;
    }
    if (auto b = state.data_buckets.lowest_non_empty(); b && open.room() > 0) {
        if (auto v = pick_victim(true, *b, C, any)) {
            for (std::uint32_t id : survivors(*v)) {
                std::uint32_t span_len = state.span(id).len;
                if (span_len <= open.room()) {
                    KLADDE_TRY(place(f, open, id));
                    stats.evacuated_bytes += span_len;
                }
            }
        }
    }
    return {};
}

// ---------------------------------------------------------------- the budget

std::optional<Offer> Store::Inner::data_offer(std::uint32_t carry, bool may_cut) {
    auto max = static_cast<std::uint32_t>(double(C) / (1.0 + opts.churn_floor));
    std::uint32_t room = C - carry;
    Offer offer;
    IdSet chosen;
    if (auto t = compaction_victim(true)) {
        std::uint32_t cov = state.live(*t).coverage;
        bool taken = false;
        if (cov <= room) {
            offer.whole.push_back(*t);
            room -= cov;
            taken = true;
        } else if (may_cut) {
            offer.cut = *t;
            room = 0;
            taken = true;
        }
        if (taken) {
            chosen.insert(*t);
            offer.tail = true;
            offer.reclaimed += C;
            offer.written += cov;
        }
    }
    while (auto b = state.data_buckets.lowest_non_empty()) {
        if (offer.cut) break;
        auto not_chosen = [&](Inner const&, std::uint32_t p) { return !chosen.contains(p); };
        auto v = pick_victim(true, *b, std::min(room, max), not_chosen);
        if (!v) break;
        std::uint32_t cov = state.live(*v).coverage;
        chosen.insert(*v);
        offer.whole.push_back(*v);
        offer.reclaimed += C - cov;
        offer.written += cov;
        room -= cov;
    }
    if (may_cut && double(room) > opts.theta * double(C) && room < max) {
        std::size_t from = Buckets::bucket_of(room + 1);
        auto too_big = [&](Inner const& i, std::uint32_t p) {
            return !chosen.contains(p) && i.state.live(p).coverage > room;
        };
        if (auto v = pick_victim(true, from, max, too_big)) {
            std::uint32_t cov = state.live(*v).coverage;
            offer.cut = *v;
            offer.reclaimed += C - cov;
            offer.written += cov;
            room = 0;
        }
    }
    if (offer.whole.empty() && !offer.cut) return std::nullopt;
    offer.fill = double(C - room) / double(C);
    return offer;
}

std::optional<Offer> Store::Inner::table_offer(double pages) {
    auto max = static_cast<std::uint32_t>(double(C) / (1.0 + opts.churn_floor));
    double room = std::min(pages, 1.0) * double(C);
    Offer offer;
    IdSet chosen;
    if (auto t = compaction_victim(false)) {
        std::uint32_t cov = state.live(*t).coverage;
        chosen.insert(*t);
        offer.whole.push_back(*t);
        offer.tail = true;
        offer.reclaimed += C;
        offer.written += cov;
        room -= cov;
    }
    while (auto b = state.table_buckets.lowest_non_empty()) {
        auto fresh = [&](Inner const&, std::uint32_t p) { return !chosen.contains(p); };
        auto limit = std::min(static_cast<std::uint32_t>(std::max(room, 0.0)), max);
        auto v = pick_victim(false, *b, limit, fresh);
        if (!v) break;
        std::uint32_t cov = state.live(*v).coverage;
        chosen.insert(*v);
        offer.whole.push_back(*v);
        offer.reclaimed += C - cov;
        offer.written += cov;
        room -= cov;
    }
    if (offer.whole.empty()) return std::nullopt;
    return offer;
}

void Store::Inner::rewrite_table_victims(Flush& f, std::vector<std::uint32_t> const& victims) {
    for (std::uint32_t v : victims) {
        cons.skip.insert(v);
        if (auto it = state.table_spans.find(v); it != state.table_spans.end())
            for (std::uint32_t id : it->second) f.stated.insert(id);
        stats.table_rewrites += 1;
    }
}

expected<void> Store::Inner::budget_loop(Flush& f, Output& out) {
    double budget = std::max(std::round(cons.budget), 0.0);
    double lambda = opts.churn_floor, theta = opts.theta;
    auto pays = [&](Offer const& o) { return o.tail || o.reclaimed >= lambda * o.written; };
    auto fills = [&](Offer const& o) { return o.tail || o.fill >= 1.0 - theta; };
    auto ratio = [](Offer const& o) { return o.reclaimed / std::max(o.written, 1.0); };
    double spent = 0.0;
    std::vector<std::uint32_t> carry;
    while (spent < budget) {
        std::uint32_t carry_len = 0;
        for (std::uint32_t id : carry) carry_len += state.span(id).len;
        std::optional<Offer> data;
        if (spent + 1.0 <= budget) data = data_offer(carry_len, spent + 2.0 <= budget);
        std::optional<Offer> offer;
        if (!carry.empty()) {
            offer = std::move(data);
        } else {
            if (data && !(pays(*data) && fills(*data))) data.reset();
            std::optional<Offer> table = table_offer(budget - spent);
            if (table && !pays(*table)) table.reset();
            if (data && table && (data->tail || (!table->tail && ratio(*data) >= ratio(*table)))) {
                offer = std::move(data);
            } else if (table) {
                spent += table->written / double(C);
                rewrite_table_victims(f, table->whole);
                continue;
            } else if (data) {
                offer = std::move(data);
            } else {
                break;
            }
        }
        spent += 1.0;
        KLADDE_TRY_ASSIGN(DataPage dp, new_data_page());
        for (std::uint32_t id : std::exchange(carry, {})) KLADDE_TRY(place(f, dp, id));
        if (offer) {
            for (std::uint32_t v : offer->whole) KLADDE_TRY(evacuate_into(f, v, dp));
            if (offer->cut) {
                KLADDE_TRY_ASSIGN(carry, evacuate_across(f, *offer->cut, dp));
            }
        }
        stats.budget_pages += 1;
        out.data.push_back(std::move(dp));
    }
    if (!carry.empty()) {
        // The budget's last page cut a span: its rest takes a page of its own.
        KLADDE_TRY_ASSIGN(DataPage dp, new_data_page());
        for (std::uint32_t id : carry) KLADDE_TRY(place(f, dp, id));
        out.data.push_back(std::move(dp));
    }
    return {};
}

std::vector<std::uint32_t> Store::Inner::cut_fillers(Flush const& f, std::size_t room_bytes) {
    // Entries restated among others take a little more than they did: keep a
    // margin.
    auto room = static_cast<std::uint32_t>(double(room_bytes) * 0.9);
    std::vector<std::uint32_t> out;
    while (auto b = state.table_buckets.lowest_non_empty()) {
        auto ok = [&](Inner const& i, std::uint32_t p) {
            auto it = i.state.table_spans.find(p);
            if (it == i.state.table_spans.end()) return false;
            return std::none_of(it->second.begin(), it->second.end(),
                                [&](std::uint32_t id) { return f.stated.contains(id); });
        };
        auto v = pick_victim(false, *b, room, ok);
        if (!v) break;
        std::uint32_t cov = state.live(*v).coverage;
        cons.skip.insert(*v);
        out.push_back(*v);
        room -= cov;
    }
    return out;
}

void Store::Inner::after_commit() {
    if (!opts.consolidate) return;
    std::uint64_t live = 0;
    for (std::size_t p = 2; p < state.pages.size(); ++p)
        if (state.pages[p].is_live()) live += state.pages[p].coverage;
    std::uint64_t pages = file_pages > 2 ? file_pages - 2 : 0;
    if (pages == 0) return;
    double fill = double(live) / (double(pages) * double(C));
    double step = std::exp(4.0 * (opts.target_fill - fill));
    double lo = opts.budget_min, hi = opts.budget_max;
    cons.budget = std::clamp(std::max(cons.budget, 1.0) * step, lo, hi);
}

void Store::Inner::fill_stats(Stats& s) const {
    s.file_pages = file_pages;
    for (std::size_t p = 2; p < state.pages.size(); ++p) {
        Page const& page = state.pages[p];
        switch (page.state) {
            case Page::State::live:
                if (page.kind == PageKind::data) {
                    s.data_pages += 1;
                    s.live_data_bytes += page.coverage;
                } else if (page.kind == PageKind::table) {
                    s.table_pages += 1;
                    s.live_table_bytes += page.coverage;
                }
                break;
            case Page::State::reusable:
            case Page::State::fallback: s.free_pages += 1; break;
            case Page::State::journal: break;
            case Page::State::filling: KLADDE_CHECK(false, "a page is filling between flushes");
        }
    }
    s.journal_pages = journal.pages.size();
    s.sequence_bytes = state.seq_len();
    s.schema_bytes = schema.size();
    s.spans = state.ids.count();
    s.budget = static_cast<std::uint64_t>(cons.budget);
}

}  // namespace kladde

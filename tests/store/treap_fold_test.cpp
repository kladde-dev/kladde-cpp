// Unit tests of the treap and of the fold.
#include <gtest/gtest.h>

#include <map>
#include <optional>

#include "../../src/store/fold.hpp"
#include "../../src/store/treap.hpp"
#include "test_util.hpp"

using namespace kladde::store;
using kladde::test::bytes;

namespace {

struct Stretch {
    std::uint64_t start = 0;
    std::uint64_t len = 0;
    std::uint64_t length() const { return len; }
    bool operator==(Stretch const&) const = default;
};

std::pair<Stretch, Stretch> cut(Stretch const& r, std::uint64_t off) {
    return {Stretch{r.start, off}, Stretch{r.start + off, r.len - off}};
}

/// The values a tree of runs over `0..` describes.
std::vector<std::uint64_t> values(Treap<Stretch> const& t, std::uint32_t root) {
    std::vector<std::uint64_t> out;
    for (std::uint32_t n : t.nodes(root))
        for (std::uint64_t v = t.item(n).start; v < t.item(n).start + t.item(n).len; ++v) out.push_back(v);
    return out;
}

}  // namespace

TEST(Treap, SplitsMovesAndFindsAgreeWithAVector) {
    kladde::test::Rng rng{0x2545F4914F6CDD1D};
    Treap<Stretch> t;
    std::uint32_t root = t.node(Stretch{0, 1000});
    std::vector<std::uint64_t> model;
    for (std::uint64_t v = 0; v < 1000; ++v) model.push_back(v);
    std::uint64_t fresh = 1000;
    for (int k = 0; k < 2000; ++k) {
        std::uint64_t len = model.size();
        std::uint64_t choice = rng.below(3);
        if (choice == 0 && len > 0) {
            // A move of a run elsewhere.
            std::uint64_t from = rng.below(len);
            std::uint64_t n = rng.below(len - from) + 1;
            auto [a, rest] = t.split_at(root, from, cut);
            auto [m, b] = t.split_at(rest, n, cut);
            std::uint32_t r = t.merge(a, b);
            std::uint64_t to = rng.below(len - n + 1);
            auto [x, y] = t.split_at(r, to, cut);
            std::uint32_t xm = t.merge(x, m);
            root = t.merge(xm, y);
            std::vector<std::uint64_t> moved(model.begin() + static_cast<std::ptrdiff_t>(from),
                                             model.begin() + static_cast<std::ptrdiff_t>(from + n));
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(from),
                        model.begin() + static_cast<std::ptrdiff_t>(from + n));
            model.insert(model.begin() + static_cast<std::ptrdiff_t>(to), moved.begin(), moved.end());
        } else if (choice == 1 && len > 0) {
            // A removal.
            std::uint64_t from = rng.below(len);
            std::uint64_t n = rng.below(len - from) + 1;
            auto [a, rest] = t.split_at(root, from, cut);
            auto [m, b] = t.split_at(rest, n, cut);
            t.free_tree(m);
            root = t.merge(a, b);
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(from),
                        model.begin() + static_cast<std::ptrdiff_t>(from + n));
        } else {
            // An insertion.
            std::uint64_t at = rng.below(len + 1);
            std::uint64_t n = rng.below(50) + 1;
            auto [a, b] = t.split_at(root, at, cut);
            std::uint32_t m = t.node(Stretch{fresh, n});
            std::uint32_t am = t.merge(a, m);
            root = t.merge(am, b);
            std::vector<std::uint64_t> ins;
            for (std::uint64_t v = fresh; v < fresh + n; ++v) ins.push_back(v);
            model.insert(model.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
            fresh += n;
        }
        t.check(root);
        ASSERT_EQ(t.sum(root), model.size());
        if (!model.empty()) {
            std::uint64_t p = rng.below(model.size());
            auto found = t.find(root, p);
            ASSERT_TRUE(found);
            EXPECT_EQ(t.item(found->first).start + found->second, model[p]);
            EXPECT_EQ(t.pos_of(found->first) + found->second, p);
        }
    }
    EXPECT_EQ(values(t, root), model);
    // Walking by `next` and `prev` visits the same nodes.
    auto order = t.nodes(root);
    std::uint32_t n = t.first(root);
    for (std::uint32_t m : order) {
        EXPECT_EQ(n, m);
        n = t.next(n);
    }
    EXPECT_EQ(n, NIL);
    n = t.last(root);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        EXPECT_EQ(n, *it);
        n = t.prev(n);
    }
    EXPECT_EQ(n, NIL);
}

TEST(Treap, NodesAreReused) {
    Treap<Stretch> t;
    std::uint32_t a = t.node(Stretch{0, 3});
    std::uint32_t b = t.node(Stretch{3, 4});
    std::uint32_t r = t.merge(a, b);
    EXPECT_EQ(t.live_nodes(), 2u);
    t.free_tree(r);
    EXPECT_EQ(t.live_nodes(), 0u);
    std::uint32_t c = t.node(Stretch{9, 1});
    EXPECT_TRUE(c == a || c == b);
}

// ---------------------------------------------------------------- fold

namespace {

/// The bytes the pieces describe, committed bytes read from `old` and new
/// ones from the arena.
std::vector<std::uint8_t> apply(std::vector<Piece> const& pieces, std::vector<std::uint8_t> const& old,
                                Records const& recs) {
    std::vector<std::uint8_t> out;
    for (Piece const& p : pieces) {
        if (!p.arena) {
            out.insert(out.end(), old.begin() + static_cast<std::ptrdiff_t>(p.start),
                       old.begin() + static_cast<std::ptrdiff_t>(p.start + p.len));
        } else {
            std::vector<std::uint8_t> b(p.len);
            recs.copy_bytes(p.start, b);
            out.insert(out.end(), b.begin(), b.end());
        }
    }
    return out;
}

using Bytes8 = std::vector<std::uint8_t>;

Bytes8 slice(Bytes8 const& v, std::uint64_t from, std::uint64_t to) {
    return Bytes8(v.begin() + static_cast<std::ptrdiff_t>(from), v.begin() + static_cast<std::ptrdiff_t>(to));
}

void splice_into(Bytes8& v, std::uint64_t pos, std::uint64_t removed, Bytes8 const& with) {
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos), v.begin() + static_cast<std::ptrdiff_t>(pos + removed));
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), with.begin(), with.end());
}

}  // namespace

TEST(Fold, TheFoldAgreesWithAVector) {
    for (std::uint64_t seed = 1; seed < 40; ++seed) {
        kladde::test::Rng rng{seed * 0x9E3779B97F4A7C15};
        Bytes8 old;
        std::uint64_t n_old = rng.below(300);
        for (std::uint64_t i = 0; i < n_old; ++i) old.push_back(static_cast<std::uint8_t>(i));
        Bytes8 model = old;
        Records recs;
        // The journal's cuts, by the number of the record that made each; a
        // paste may name one of any transaction.
        std::vector<std::optional<std::pair<std::uint64_t, Bytes8>>> cuts;
        for (int k = 0; k < 200; ++k) {
            if (rng.below(5) == 0) recs.mark_start(recs.end().at);
            std::uint64_t at = recs.end().at;
            std::uint64_t len = model.size();
            std::uint64_t choice = rng.below(4);
            Record r;
            if (choice == 0 && len > 0) {
                std::uint64_t pos = rng.below(len);
                std::uint64_t n = rng.below(len - pos) + 1;
                Bytes8 fresh;
                for (std::uint64_t i = 0; i < n; ++i) fresh.push_back(static_cast<std::uint8_t>(rng.below(256)));
                cuts.emplace_back(std::pair{at, slice(model, pos, pos + n)});
                splice_into(model, pos, n, fresh);
                r = Record::write(pos, recs.stash(fresh));
            } else if (choice == 1) {
                std::uint64_t pos = rng.below(len + 1);
                std::uint64_t removed = rng.below(len - pos + 1);
                Bytes8 fresh;
                std::uint64_t n = rng.below(20);
                for (std::uint64_t i = 0; i < n; ++i) fresh.push_back(static_cast<std::uint8_t>(rng.below(256)));
                Bytes8 gone = slice(model, pos, pos + removed);
                splice_into(model, pos, removed, fresh);
                if (removed > 0) cuts.emplace_back(std::pair{at, gone});
                r = Record::splice(pos, removed, recs.stash(fresh));
            } else if (choice == 2 && len > 0) {
                std::uint64_t from = rng.below(len);
                std::uint64_t n = rng.below(len - from) + 1;
                std::uint64_t to = rng.below(len - n + 1);
                Bytes8 m = slice(model, from, from + n);
                splice_into(model, from, n, {});
                splice_into(model, to, 0, m);
                r = Record::move(from, n, to);
            } else {
                std::optional<std::size_t> found;
                for (std::size_t i = cuts.size(); i-- > 0;) {
                    if (cuts[i]) {
                        found = i;
                        break;
                    }
                }
                if (!found) continue;
                auto [cut_at, cut] = *cuts[*found];
                cuts[*found].reset();
                std::uint64_t pos = rng.below(len + 1);
                std::uint64_t removed = rng.below(len - pos + 1);
                splice_into(model, pos, removed, cut);
                r = Record::paste(pos, removed, cut_at, cut.size(), std::nullopt);
            }
            recs.push(r);
        }
        auto pieces = fold(recs, recs.front(), recs.end(), old.size(), 0);
        ASSERT_TRUE(pieces) << "seed " << seed;
        EXPECT_EQ(apply(*pieces, old, recs), model) << "seed " << seed;
        // Adjacent pieces never continue each other.
        for (std::size_t i = 1; i < pieces->size(); ++i)
            EXPECT_FALSE((*pieces)[i - 1].continued_by((*pieces)[i]));
    }
}

TEST(Fold, ARecordPastTheEndIsRefused) {
    Records recs;
    Bytes b = recs.stash(bytes("ab"));
    recs.push(Record::write(9, b));
    EXPECT_FALSE(fold(recs, recs.front(), recs.end(), 10, 0));
    EXPECT_TRUE(fold(recs, recs.front(), recs.end(), 11, 0));
}

TEST(Fold, ACutFromAnotherTransactionOfTheJournalIsPasted) {
    Bytes8 old = bytes("abcde");
    Records recs;
    recs.mark_start(0);
    Bytes b = recs.stash({});
    recs.push(Record::splice(0, 2, b));
    recs.mark_start(1);
    recs.push(Record::paste(3, 0, 0, 2, std::nullopt));
    auto pieces = fold(recs, recs.front(), recs.end(), 5, 0);
    ASSERT_TRUE(pieces);
    EXPECT_EQ(apply(*pieces, old, recs), bytes("cdeab"));
    // Nothing of the paste is new bytes: the moved run is committed.
    for (Piece const& p : *pieces) EXPECT_FALSE(p.arena);
}

TEST(Fold, APasteOfAnUnknownCutIsRefusedAndAnEndedOneFallsBack) {
    Records recs;
    Bytes fallback = recs.stash(bytes("xy"));
    recs.push(Record::paste(0, 0, 7, 2, fallback));
    // Record 7 is not in this journal, which starts at record 0 ...
    EXPECT_FALSE(fold(recs, recs.front(), recs.end(), 3, 0));
    // ... but in one that starts at record 8, its cut has ended, and the
    // paste writes its own bytes.
    auto pieces = fold(recs, recs.front(), recs.end(), 3, 8);
    ASSERT_TRUE(pieces);
    EXPECT_EQ(apply(*pieces, bytes("abc"), recs), bytes("xyabc"));
}

// Differential tests: random writes, splices, moves, and cuts pasted within
// transactions and across them, against a plain byte vector, with flushes,
// reopenings and new schema chains in between.
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <kladde/store.hpp>

#include "test_util.hpp"

using namespace kladde;
using test::boxed;
using test::bytes;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct OracleRng : test::Rng {
    Bytes fill(std::size_t n) {
        auto f = static_cast<std::uint8_t>(below(255) + 1);
        Bytes out(n);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>(f + (i % 7));
        return out;
    }
};

Bytes slice(Bytes const& v, std::uint64_t from, std::uint64_t to) {
    return Bytes(v.begin() + static_cast<std::ptrdiff_t>(from), v.begin() + static_cast<std::ptrdiff_t>(to));
}

/// Replaces `removed` bytes of `v` at `pos` with `with`; returns what it
/// removed.
Bytes splice_into(Bytes& v, std::uint64_t pos, std::uint64_t removed, Bytes const& with) {
    Bytes gone = slice(v, pos, pos + removed);
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(pos), v.begin() + static_cast<std::ptrdiff_t>(pos + removed));
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(pos), with.begin(), with.end());
    return gone;
}

struct World {
    MemoryStorage storage;
    Store store;
    Bytes model;
    Bytes schema;
    Options opts;
    /// A cut held across operations, and the bytes it holds.
    std::optional<std::pair<CutId, Bytes>> held;

    explicit World(Options o)
        : storage(), store(Store::create(boxed(storage), o).value()), opts(o) {}

    void verify(std::string const& what) {
        store.check();
        Bytes got = store.read_all().value();
        if (got != model) {
            std::size_t first = 0;
            while (first < got.size() && first < model.size() && got[first] == model[first]) ++first;
            FAIL() << what << ": the sequence differs at " << first << " (lengths " << got.size()
                   << " and " << model.size() << ")\n"
                   << store.describe();
        }
        ASSERT_EQ(store.schema(), schema) << what << ": the schema";
    }

    void reopen() {
        // A cut belongs to its session; what it held stays removed.
        held.reset();
        MemoryStorage fresh = MemoryStorage::from_image(storage.image());
        store = Store::open(boxed(fresh), opts).value();
        storage = fresh;
        ASSERT_EQ(store.size(), model.size());
    }

    /// One random operation, applied to the store and the model alike.
    void step(OracleRng& rng, std::uint64_t max_len) {
        std::uint64_t len = model.size();
        std::uint64_t choice = rng.below(12);
        if (choice <= 2 && len > 0) {
            std::uint64_t pos = rng.below(len);
            std::uint64_t n = rng.below(std::min<std::uint64_t>(len - pos, 600)) + 1;
            Bytes b = rng.fill(n);
            splice_into(model, pos, n, b);
            store.write(pos, b).value();
        } else if (choice >= 3 && choice <= 6) {
            // Mostly keystrokes and short deletions near one place, now and
            // then a long run, and the sequence kept below `max_len`.
            std::uint64_t pos = rng.below(len + 1);
            std::uint64_t removed = rng.below(4) == 0
                                        ? rng.below(std::min<std::uint64_t>(len - pos, 3000) + 1)
                                        : rng.below(std::min<std::uint64_t>(len - pos, 3) + 1);
            std::uint64_t kind = rng.below(8);
            std::uint64_t n = kind == 0 ? rng.below(9000) : kind <= 3 ? rng.below(40) : 1;
            if (len - removed + n > max_len) n = 0;
            Bytes b = rng.fill(n);
            Bytes gone = splice_into(model, pos, removed, b);
            auto cut = store.splice(pos, removed, b).value();
            // Now and then, hold on to the cut, to paste it in a later
            // transaction, perhaps after a flush has ended it.
            if (cut && !held && rng.below(3) == 0) held = std::pair{*cut, gone};
        } else if (choice == 7 && len > 0) {
            std::uint64_t from = rng.below(len);
            std::uint64_t n = rng.below(std::min<std::uint64_t>(len - from, 5000)) + 1;
            std::uint64_t to = rng.below(len - n + 1);
            Bytes m = splice_into(model, from, n, {});
            splice_into(model, to, 0, m);
            store.move(from, n, to).value();
        } else if (choice == 8 && len > 1) {
            // Two cuts in one transaction, pasted in the other order, the
            // second over bytes it replaces, and between them the bytes a
            // write overwrote.
            auto r = store.atomically([&]() -> expected<void> {
                std::uint64_t l = model.size();
                std::uint64_t a = rng.below(l);
                std::uint64_t an = rng.below(std::min<std::uint64_t>(l - a, 2000)) + 1;
                Bytes ca = splice_into(model, a, an, {});
                KLADDE_TRY_ASSIGN(auto cut_a, store.splice(a, an, {}));
                l = model.size();
                std::uint64_t b = rng.below(l + 1);
                std::uint64_t bn = rng.below(std::min<std::uint64_t>(l - b, 2000) + 1);
                Bytes cb = splice_into(model, b, bn, {});
                KLADDE_TRY_ASSIGN(auto cut_b, store.splice(b, bn, bytes("x")));
                model.insert(model.begin() + static_cast<std::ptrdiff_t>(b), 'x');
                l = model.size();
                std::uint64_t w = rng.below(l);
                std::uint64_t wn = rng.below(std::min<std::uint64_t>(l - w, 50)) + 1;
                Bytes fresh = rng.fill(wn);
                Bytes cw = splice_into(model, w, wn, fresh);
                KLADDE_TRY_ASSIGN(auto cut_w, store.write(w, fresh));
                l = model.size();
                std::uint64_t p = rng.below(l + 1);
                splice_into(model, p, 0, ca);
                KLADDE_TRY(store.paste(p, 0, *cut_a, ca));
                l = model.size();
                std::uint64_t s = rng.below(l + 1);
                splice_into(model, s, 0, cw);
                KLADDE_TRY(store.paste(s, 0, *cut_w, cw));
                if (cut_b) {
                    l = model.size();
                    std::uint64_t q = rng.below(l + 1);
                    std::uint64_t rr = rng.below(std::min<std::uint64_t>(l - q, 10) + 1);
                    splice_into(model, q, rr, cb);
                    KLADDE_TRY(store.paste(q, rr, *cut_b, cb));
                }
                return {};
            });
            r.value();
        } else if (choice == 9) {
            schema = rng.fill(rng.below(400));
            store.set_schema(schema);
        } else if (choice == 10) {
            store.flush().value();
            verify("after a flush");
        } else {
            // Also the arms above whose guard failed, as kladde-rs's `_` arm
            // takes them, so that seeds draw the same operations.
            if (held) {
                auto [cut, b] = *held;
                held.reset();
                std::uint64_t pos = rng.below(model.size() + 1);
                splice_into(model, pos, 0, b);
                store.paste(pos, 0, cut, b).value();
            }
            if (rng.below(3) == 0) {
                store.flush().value();
                reopen();
                verify("after reopening");
            }
        }
    }
};

void run(std::uint64_t seed, Options opts, std::size_t steps, std::uint64_t max_len) {
    // The first failing seed is the one to report.
    if (::testing::Test::HasFailure()) return;
    OracleRng rng{{(seed * 0x9E3779B97F4A7C15) | 1}};
    World w(opts);
    for (std::size_t k = 0; k < steps; ++k) {
        w.step(rng, max_len);
        if (::testing::Test::HasFatalFailure() || ::testing::Test::HasNonfatalFailure()) {
            ADD_FAILURE() << "seed " << seed << ", step " << k;
            return;
        }
    }
    w.store.flush().value();
    w.verify("seed " + std::to_string(seed) + ", at the end");
    w.reopen();
    w.verify("seed " + std::to_string(seed) + ", reopened at the end");
}

}  // namespace

TEST(Oracle, ShortSequences) {
    for (std::uint64_t seed = 1; seed < 60; ++seed) run(seed, Options{}, 300, 20'000);
}

TEST(Oracle, LongSequences) {
    for (std::uint64_t seed = 1; seed < 12; ++seed) run(seed, Options{}, 600, 400'000);
}

TEST(Oracle, WithoutConsolidation) {
    Options opts;
    opts.consolidate = false;
    for (std::uint64_t seed = 1; seed < 30; ++seed) run(seed, opts, 300, 50'000);
}

TEST(Oracle, WithCommitsOnAWorker) {
    Options opts;
    opts.journal_budget_pages = 1;
    opts.sync_on_worker = true;
    std::uint64_t on_worker = 0;
    for (std::uint64_t seed = 1; seed < 30; ++seed) {
        OracleRng rng{{(seed * 0x9E3779B9) | 1}};
        World w(opts);
        for (int k = 0; k < 300; ++k) {
            w.step(rng, 50'000);
            on_worker += w.store.stats().commits_on_worker;
            if (HasFailure()) {
                ADD_FAILURE() << "seed " << seed << ", step " << k;
                return;
            }
        }
        w.store.flush().value();
        w.verify("seed " + std::to_string(seed) + ", at the end");
        w.reopen();
        w.verify("seed " + std::to_string(seed) + ", reopened at the end");
    }
    EXPECT_GT(on_worker, 0u) << "no flush handed its commit to a worker";
}

TEST(Oracle, WithATinyJournal) {
    Options opts;
    opts.journal_budget_pages = 1;
    for (std::uint64_t seed = 1; seed < 30; ++seed) run(seed, opts, 300, 50'000);
}

TEST(Oracle, WithPayloadPagesForNearlyEveryWrite) {
    Options opts;
    opts.payload_threshold = 16;
    for (std::uint64_t seed = 1; seed < 40; ++seed) run(seed, opts, 300, 50'000);
}

TEST(Oracle, WithPayloadPagesALargeJournalAndCommitsOnAWorker) {
    Options opts;
    opts.payload_threshold = 100;
    opts.journal_budget_pages = 32;
    opts.sync_on_worker = true;
    for (std::uint64_t seed = 1; seed < 40; ++seed) run(seed, opts, 300, 50'000);
}

TEST(Oracle, WithEagerMergesAndATinyHeader) {
    // Every flush walks the whole sequence, everything is cold at once, and
    // short spans merge readily.
    Options opts;
    opts.cold_flushes = 0;
    opts.walk = 100'000;
    opts.span_min = 4000;
    opts.mu = 0.0;
    opts.rewrite_below = 0;
    opts.inline_threshold = 1;
    for (std::uint64_t seed = 1; seed < 30; ++seed) run(seed, opts, 300, 50'000);
}

TEST(Oracle, ACutLivesAsLongAsItsJournal) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    store.splice(0, 0, bytes("abcdef")).value();
    store.flush().value();
    // Cut in one transaction, pasted in another: a paste, which writes none
    // of the cut's bytes.
    CutId cut = *store.splice(0, 2, {}).value();
    store.write(0, bytes("C")).value();
    EXPECT_TRUE(store.is_alive(cut));
    std::uint64_t written = store.stats().journal_bytes;
    store.paste(4, 0, cut, bytes("ab")).value();
    EXPECT_LT(store.stats().journal_bytes - written, 2u + 9u);
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("Cdefab"));
    // A flush ends the cuts of the journal it folds; a paste then writes the
    // bytes it was given.
    cut = *store.splice(0, 1, {}).value();
    store.flush().value();
    EXPECT_FALSE(store.is_alive(cut));
    store.paste(5, 0, cut, bytes("C")).value();
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("defabC"));
    // A write's cut, pasted outside any transaction.
    cut = *store.write(0, bytes("D")).value();
    store.paste(6, 0, cut, bytes("d")).value();
    store = Store::open(boxed(storage)).value();
    EXPECT_EQ(store.read_all().value(), bytes("DefabCd"));
}

TEST(Oracle, APasteWhoseCutEndsBeforeItsTransactionIsAppendedWritesItsBytes) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    store.splice(0, 0, bytes("abcdef")).value();
    CutId cut = *store.splice(0, 2, {}).value();
    // The paste is recorded while its cut lives, in a transaction that an
    // explicit flush holds back past the journal it folds.
    store.begin_transaction().value();
    store.paste(4, 0, cut, bytes("ab")).value();
    store.flush().value();
    store.end_transaction().value();
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("cdefab"));
    store = Store::open(boxed(storage)).value();
    EXPECT_EQ(store.read_all().value(), bytes("cdefab"));
}

TEST(Oracle, ASpliceOfEqualLengthsCutsWhatItOverwrites) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, bytes("abcdef")).value();
    store.atomically([&]() -> expected<void> {
             KLADDE_TRY_ASSIGN(auto old, store.splice(1, 2, bytes("XY")));
             EXPECT_EQ(store.size(), 6u);
             return store.paste(6, 0, *old, bytes("bc"));
         })
        .value();
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("aXYdefbc"));
}

TEST(Oracle, AFailedTransactionLeavesNothing) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, bytes("abc")).value();
    auto r = store.atomically([&]() -> expected<void> {
        KLADDE_TRY(store.splice(0, 1, bytes("xyz")));
        KLADDE_TRY(store.write(100, bytes("past the end")));
        return {};
    });
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error().code, errc::out_of_bounds);
    EXPECT_EQ(store.size(), 3u);
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("abc"));
}

TEST(Oracle, ASpliceStatesAFewSpansHoweverLongTheSequence) {
    // Without consolidation, whose compaction mode would move the file's tail
    // down over the pages the first megabyte's journal left free.
    Options opts;
    opts.consolidate = false;
    Store store = Store::create(std::make_unique<MemoryStorage>(), opts).value();
    store.splice(0, 0, Bytes(1 << 20, 7)).value();
    store.flush().value();
    Stats before = store.stats();
    store.splice(1000, 0, bytes("a long sequence, a short splice")).value();
    store.flush().value();
    Stats after = store.stats();
    // The span the splice lands in is split, and the new bytes are inline: no
    // data page and no leaf is written, only the header.
    EXPECT_EQ(after.span_splits, before.span_splits + 1);
    EXPECT_EQ(after.data_pages_written, before.data_pages_written);
    EXPECT_EQ(after.table_pages_written, before.table_pages_written);
    EXPECT_EQ(after.headers_written, before.headers_written + 1);
    store.check();
}

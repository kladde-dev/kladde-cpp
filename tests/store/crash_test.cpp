// Power cuts at every kind of moment: recovery must land exactly on a
// transaction boundary no earlier than the last completed flush.
#include <gtest/gtest.h>

#include <kladde/store.hpp>

#include "test_util.hpp"

using namespace kladde;
using test::boxed;

namespace {

using Bytes = std::vector<std::uint8_t>;

/// The sequence and the schema chain.
struct Model {
    Bytes seq;
    Bytes schema;
    bool operator==(Model const&) const = default;
};

Model recovered(Bytes image, Options const& opts) {
    auto store = Store::open(std::make_unique<MemoryStorage>(MemoryStorage::from_image(std::move(image))), opts);
    EXPECT_TRUE(store) << "a crashed file opens: " << store.error().what();
    if (!store) return {};
    store->check();
    return {store->read_all().value(), store->schema()};
}

/// Random operations until the storage fails, then power cuts; with `mixed`,
/// batches, transactions with cuts and pastes, and splices of up to two pages
/// too.
void run(std::uint64_t seed, Options const& opts, bool mixed) {
    // The first failing seed is the one to report.
    if (::testing::Test::HasFailure()) return;
    test::Rng rng{(seed * 0x9E3779B97F4A7C15) | 1};
    MemoryStorage storage = MemoryStorage::with_crash_tracking();
    Store store = Store::create(boxed(storage), opts).value();
    // The model after each transaction; `history[flushed]` is the state the
    // last completed flush made durable.
    Model model;
    std::vector<Model> history{model};
    std::size_t flushed = 0;
    bool in_batch = false;
    storage.fail_after(rng.below(400) + 1);
    for (int step = 0; step < 300; ++step) {
        std::uint64_t len = model.seq.size();
        std::uint64_t kind = rng.below(mixed ? 11 : 8);
        expected<void> r;
        auto erase = [&](std::uint64_t from, std::uint64_t n) {
            Bytes gone(model.seq.begin() + static_cast<std::ptrdiff_t>(from),
                       model.seq.begin() + static_cast<std::ptrdiff_t>(from + n));
            model.seq.erase(model.seq.begin() + static_cast<std::ptrdiff_t>(from),
                            model.seq.begin() + static_cast<std::ptrdiff_t>(from + n));
            return gone;
        };
        auto insert = [&](std::uint64_t at, Bytes const& b) {
            model.seq.insert(model.seq.begin() + static_cast<std::ptrdiff_t>(at), b.begin(), b.end());
        };
        if (kind == 8) {
            in_batch = !in_batch;
            r = in_batch ? store.begin_batch() : store.end_batch();
        } else if (kind == 9 && len > 0) {
            // A transaction: a cut, a write, and the cut pasted back
            // elsewhere, all or none of it.
            std::uint64_t a = rng.below(len);
            std::uint64_t n = rng.below(std::min<std::uint64_t>(len - a, 900)) + 1;
            std::uint64_t w = rng.below(len - n + 1);
            auto fill = static_cast<std::uint8_t>(rng.below(250) + 1);
            std::uint64_t to = rng.below(len - n + 1);
            Bytes cut = erase(a, n);
            std::uint64_t wn = std::min<std::uint64_t>(model.seq.size() - w, 3);
            std::fill_n(model.seq.begin() + static_cast<std::ptrdiff_t>(w), wn, fill);
            insert(to, cut);
            r = store.atomically([&]() -> expected<void> {
                KLADDE_TRY_ASSIGN(auto c, store.splice(a, n, {}));
                KLADDE_TRY(store.write(w, Bytes(wn, fill)));
                return store.paste(to, 0, *c, cut);
            });
        } else if (kind == 10) {
            // The fill is drawn first, as kladde-rs evaluates `vec![x; n]`.
            auto fill = static_cast<std::uint8_t>(rng.below(250) + 1);
            model.schema = Bytes(rng.below(300), fill);
            store.set_schema(model.schema);
        } else if (kind <= 2 && len > 0) {
            std::uint64_t pos = rng.below(len);
            std::uint64_t n = rng.below(std::min<std::uint64_t>(len - pos, 900)) + 1;
            auto fill = static_cast<std::uint8_t>(rng.below(250) + 1);
            std::fill_n(model.seq.begin() + static_cast<std::ptrdiff_t>(pos), n, fill);
            auto w = store.write(pos, Bytes(n, fill));
            r = w ? expected<void>{} : expected<void>(w.error());
        } else if (kind >= 3 && kind <= 5) {
            std::uint64_t pos = rng.below(len + 1);
            std::uint64_t removed = rng.below(std::min<std::uint64_t>(len - pos, 500) + 1);
            std::uint64_t max = mixed && rng.below(10) == 0 ? 8192 : 900;
            auto fill = static_cast<std::uint8_t>(rng.below(250) + 1);
            Bytes b(rng.below(max), fill);
            erase(pos, removed);
            insert(pos, b);
            auto s = store.splice(pos, removed, b);
            r = s ? expected<void>{} : expected<void>(s.error());
        } else if (kind == 6 && len > 0) {
            std::uint64_t from = rng.below(len);
            std::uint64_t n = rng.below(len - from) + 1;
            std::uint64_t to = rng.below(len - n + 1);
            Bytes m = erase(from, n);
            insert(to, m);
            r = store.move(from, n, to);
        } else {
            r = store.flush();
            if (r) flushed = history.size() - 1;
        }
        // A failed operation may or may not have reached the journal, so both
        // the state before it and the state after it are boundaries. A new
        // schema reaches the file only with a flush, so the states with and
        // without it are both boundaries until then.
        if (history.back() != model) history.push_back(model);
        if (!r) break;
    }
    std::size_t n = storage.unsynced_writes();
    std::vector<Bytes> images{storage.crash_image([](std::size_t) { return false; }),
                              storage.crash_image([](std::size_t) { return true; })};
    for (int k = 0; k < 4; ++k) {
        std::vector<bool> keep;
        for (std::size_t i = 0; i < n; ++i) keep.push_back(rng.below(2) == 0);
        // A commit on a worker may still be writing: what it writes after the
        // count is lost, as a power cut now would lose it.
        images.push_back(storage.crash_image([&](std::size_t i) { return i < keep.size() && keep[i]; }));
    }
    for (Bytes& image : images) {
        Model got = recovered(std::move(image), opts);
        if (::testing::Test::HasFailure()) {
            ADD_FAILURE() << "seed " << seed;
            return;
        }
        // The schema chain is written by flushes alone, so a state matches a
        // boundary whose sequence is the same, and whose schema is the one the
        // last flush before it wrote.
        std::optional<std::size_t> match;
        for (std::size_t k = history.size(); k-- > 0;) {
            if (history[k].seq != got.seq) continue;
            bool schema_seen = false;
            for (std::size_t e = 0; e <= k; ++e) schema_seen |= history[e].schema == got.schema;
            if (schema_seen) {
                match = k;
                break;
            }
        }
        ASSERT_TRUE(match) << "seed " << seed << ": the recovered state is no transaction boundary";
        ASSERT_GE(*match, flushed) << "seed " << seed << ": recovered transaction " << *match
                                   << ", but " << flushed << " were flushed";
    }
}

}  // namespace

TEST(Crash, PowerCutsLandOnTransactionBoundaries) {
    Options opts;
    opts.consolidate = false;
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, opts, false);
}

TEST(Crash, PowerCutsWithConsolidation) {
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, Options{}, false);
}

TEST(Crash, PowerCutsWithATinyJournal) {
    Options opts;
    opts.journal_budget_pages = 1;
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, opts, false);
}

namespace {

/// What survives when a flush's header is lost but the transactions appended
/// after it are not: the image as of the flush's `fsync`, plus every later
/// write but the header's. Returns what reopening finds, and what holds
/// every transaction.
std::pair<Bytes, Bytes> lost_header(Options const& opts) {
    MemoryStorage storage = MemoryStorage::with_crash_tracking();
    Store store = Store::create(boxed(storage), opts).value();
    // Flushes that free pages, so that a later one has a reusable page for
    // the next journal.
    for (std::uint8_t i = 0; i < 6; ++i) {
        store.splice(0, store.size(), Bytes(3 * 4096, i)).value();
        store.flush().value();
    }
    store.splice(0, 0, test::bytes("before")).value();
    store.flush().value();
    store.splice(0, 0, test::bytes("after ")).value();
    Bytes expected = store.read_all().value();
    Bytes after = test::bytes("after ");
    expected.insert(expected.begin(), after.begin(), after.end());
    // The first write after the flush's `fsync` is its header.
    Bytes image = storage.crash_image([](std::size_t i) { return i != 0; });
    Store reopened = Store::open(boxed(MemoryStorage::from_image(image)), opts).value();
    reopened.check();
    return {reopened.read_all().value(), expected};
}

}  // namespace

/// Flushes that mutations trigger sync and write their header on a thread of
/// their own, while the next transactions are appended: power cuts at every
/// interleaving of the two must still land on a transaction boundary no
/// earlier than the last flush that returned.
TEST(Crash, PowerCutsWithCommitsOnAWorker) {
    Options opts;
    opts.journal_budget_pages = 1;
    opts.sync_on_worker = true;
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, opts, true);
}

/// Payload pages for nearly every write and splice, written without an
/// `fsync` beside the journal's appends: a power cut that keeps a
/// transaction's records but not its payload pages loses the transaction.
TEST(Crash, PowerCutsWithPayloadPages) {
    Options opts;
    opts.payload_threshold = 32;
    opts.journal_budget_pages = 16;
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, opts, true);
}

TEST(Crash, PowerCutsWithPayloadPagesAndCommitsOnAWorker) {
    Options opts;
    opts.payload_threshold = 32;
    opts.journal_budget_pages = 4;
    opts.sync_on_worker = true;
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, opts, true);
}

TEST(Crash, ASealLeadsRecoveryToWhatWasAppendedAfterALostHeader) {
    auto [found, expected] = lost_header(Options{});
    EXPECT_EQ(found, expected);
}

TEST(Crash, WithoutASealWhatWasAppendedAfterALostHeaderIsLost) {
    Options opts;
    opts.seal = false;
    auto [found, expected] = lost_header(opts);
    EXPECT_EQ(found, Bytes(expected.begin() + 6, expected.end()));
}

/// With a journal of one page, batches are cut and flushed often, and a
/// splice of two pages is flushed before and after.
TEST(Crash, PowerCutsWithBatchesCutsAndOverlongSplicesInATinyJournal) {
    Options opts;
    opts.journal_budget_pages = 1;
    for (std::uint64_t seed = 1; seed < 150; ++seed) run(seed, opts, true);
}

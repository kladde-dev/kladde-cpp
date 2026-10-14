// Consolidation keeps a file near its sequence's length under sustained
// edits, and the span policy keeps the spans few, without changing what the
// file reads.
#include <gtest/gtest.h>

#include <kladde/store.hpp>

#include "test_util.hpp"

using namespace kladde;
using test::boxed;

namespace {

using Bytes = std::vector<std::uint8_t>;

/// Checks that the store, and the file reopened, read `model`.
void verify(Store const& store, MemoryStorage const& storage, Options const& opts, Bytes const& model) {
    store.check();
    EXPECT_TRUE(store.read_all().value() == model) << "the sequence differs";
    Store reopened = Store::open(boxed(MemoryStorage::from_image(storage.image())), opts).value();
    reopened.check();
    EXPECT_TRUE(reopened.read_all().value() == model) << "the sequence differs after reopening";
}

/// Overwrites random ranges of a sequence of `size` bytes, flushing every
/// `per_flush` writes.
Stats overwrite(Options opts, std::size_t size, std::size_t write, std::size_t per_flush,
                std::size_t flushes) {
    test::Rng rng{0x2545F4914F6CDD1D};
    MemoryStorage storage;
    Store store = Store::create(boxed(storage), opts).value();
    Bytes model(size, 0);
    store.splice(0, 0, model).value();
    for (std::size_t f = 0; f < flushes; ++f) {
        for (std::size_t k = 0; k < per_flush; ++k) {
            std::size_t len = rng.below(write) + 1;
            std::size_t off = rng.below(size - len + 1);
            std::fill_n(model.begin() + static_cast<std::ptrdiff_t>(off), len,
                        static_cast<std::uint8_t>(f % 251 + 1));
            store.write(off, std::span(model).subspan(off, len)).value();
        }
        store.flush().value();
    }
    verify(store, storage, opts, model);
    return store.stats();
}

double data_fill(Stats const& s) {
    return double(s.live_data_bytes) / (double(s.data_pages) * double(max_page_content));
}

/// Types into a sequence of `size` bytes keystroke by keystroke at a cursor
/// that jumps now and then, flushing every `per_flush` keystrokes.
Stats typing(Options opts, std::size_t size, std::uint64_t keystrokes, std::uint64_t per_flush) {
    test::Rng rng{0x9E3779B97F4A7C15};
    MemoryStorage storage;
    Store store = Store::create(boxed(storage), opts).value();
    Bytes model(size, ' ');
    store.splice(0, 0, model).value();
    std::uint64_t cursor = 0;
    for (std::uint64_t k = 0; k < keystrokes; ++k) {
        if (rng.below(500) == 0) cursor = rng.below(model.size() + 1);
        if (rng.below(10) == 0 && cursor > 0) {
            cursor -= 1;
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(cursor));
            store.splice(cursor, 1, {}).value();
        } else {
            auto c = static_cast<std::uint8_t>('a' + k % 26);
            model.insert(model.begin() + static_cast<std::ptrdiff_t>(cursor), c);
            store.splice(cursor, 0, std::span(&c, 1)).value();
            cursor += 1;
        }
        if (k % per_flush == per_flush - 1) store.flush().value();
    }
    store.flush().value();
    verify(store, storage, opts, model);
    return store.stats();
}

/// Writes a sequence of `n` pages, cuts out all but every fourth, then runs
/// `flushes` flushes of a small overwrite each. Returns the file's length in
/// pages before and after.
std::pair<std::uint64_t, std::uint64_t> shrink(Options opts, std::size_t n, std::size_t flushes) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage), opts).value();
    std::size_t page = 4000;
    Bytes model(n * page);
    for (std::size_t i = 0; i < model.size(); ++i) model[i] = static_cast<std::uint8_t>(i / page % 251 + 1);
    store.begin_batch().value();
    for (std::size_t i = 0; i < n; ++i)
        store.splice(i * page, 0, std::span(model).subspan(i * page, page)).value();
    store.end_batch().value();
    store.flush().value();
    std::uint64_t before = storage.size().value() / page_size;
    for (std::size_t i = n; i-- > 0;) {
        if (i % 4 != 0) {
            model.erase(model.begin() + static_cast<std::ptrdiff_t>(i * page),
                        model.begin() + static_cast<std::ptrdiff_t>((i + 1) * page));
            store.splice(i * page, page, {}).value();
        }
    }
    for (std::size_t f = 0; f < flushes; ++f) {
        std::fill_n(model.begin() + static_cast<std::ptrdiff_t>(f), 8, static_cast<std::uint8_t>(f));
        store.write(f, std::span(model).subspan(f, 8)).value();
        store.flush().value();
    }
    store.close().value();
    verify(store, storage, opts, model);
    return {before, storage.size().value() / page_size};
}

}  // namespace

TEST(Consolidation, EvacuationKeepsDataPagesFull) {
    Options off;
    off.consolidate = false;
    Stats without = overwrite(off, 512 * 1024, 600, 32, 300);
    Stats with = overwrite(Options{}, 512 * 1024, 600, 32, 300);
    EXPECT_GT(with.evacuated_pages, 0u);
    EXPECT_LT(data_fill(without), 0.6);
    EXPECT_GT(data_fill(with), 0.75);
    EXPECT_LT(with.data_pages * 3, without.data_pages * 2);
}

TEST(Consolidation, LeavesAreDrainedAndStayFull) {
    Stats with = overwrite(Options{}, 512 * 1024, 600, 32, 300);
    EXPECT_GT(with.table_rewrites, 0u);
    EXPECT_GT(with.live_fraction(), 0.7);
}

TEST(Consolidation, MergesKeepTheSpansOfTypedTextFew) {
    Options off;
    off.span_min = 0;
    off.walk = 0;
    Stats without = typing(off, 64 * 1024, 30'000, 50);
    Stats with = typing(Options{}, 64 * 1024, 30'000, 50);
    EXPECT_TRUE(with.evict_merges > 0 && with.walk_merges > 0);
    EXPECT_LT(with.spans * 2, without.spans);
    // The description stays a small share of the bytes it describes.
    EXPECT_LT(double(with.live_table_bytes), 0.05 * double(with.sequence_bytes));
}

TEST(Consolidation, CompactionMovesContentOffTheEnd) {
    Options off;
    off.hole_share = 1.0;
    auto [before, without] = shrink(off, 400, 60);
    auto [ignored, with] = shrink(Options{}, 400, 60);
    (void)ignored;
    EXPECT_GT(without * 10, before * 7) << without << " of " << before << " pages";
    EXPECT_LT(with * 2, before) << with << " of " << before << " pages";
}

// Payload pages: large bytes written once, into data pages the journal
// names, which the flush that folds the journal takes into the file.
#include <gtest/gtest.h>

#include <algorithm>

#include <kladde/store.hpp>

#include "test_util.hpp"

using namespace kladde;
using test::boxed;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t PAGE = 4096;

/// Bytes no run of zeros hides, which differ from page to page.
Bytes content(std::size_t n, std::uint8_t seed) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i)
        b[i] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(i) * 31 + seed) | 1);
    return b;
}

Options budget(std::uint32_t pages) {
    Options opts;
    opts.journal_budget_pages = pages;
    return opts;
}

}  // namespace

TEST(Payload, CreatingALargeFileWritesItsContentOnce) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    Bytes bytes = content(4'361'568, 7);
    store.splice(0, 0, bytes).value();
    store.flush().value();
    Stats s = store.stats();
    // The bytes went to payload pages, which the flush took as they lie.
    EXPECT_GE(s.payload_bytes, bytes.size() - 2048);
    EXPECT_EQ(s.adopted_bytes, s.payload_bytes);
    EXPECT_LE(s.data_pages_written, 1u);
    EXPECT_LT(s.journal_bytes, 20'000u);
    store.close().value();
    std::uint64_t pages = storage.size().value() / PAGE;
    std::uint64_t content_pages = (bytes.size() + max_page_content - 1) / max_page_content;
    EXPECT_LE(pages, content_pages + 8) << pages << " pages for " << content_pages;
    Store reopened = Store::open(boxed(storage)).value();
    reopened.check();
    EXPECT_EQ(reopened.read_all().value(), bytes);
}

TEST(Payload, PayloadPagesCountTowardTheBudgetSoALargeWriteIsFlushedAtOnce) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    store.splice(0, 0, content(1000, 1)).value();
    EXPECT_EQ(store.stats().flushes, 0u);
    store.splice(500, 0, content(200'000, 2)).value();
    // One flush before it, for the journal that held the first splice, and
    // one after it.
    EXPECT_EQ(store.stats().flushes, 2u);
    Store after = Store::open(boxed(MemoryStorage::from_image(storage.image()))).value();
    EXPECT_EQ(after.size(), 201'000u);
}

TEST(Payload, APayloadPageThatNeverReachedTheDiskDropsItsTransaction) {
    Options opts = budget(16);
    MemoryStorage storage = MemoryStorage::with_crash_tracking();
    Store store = Store::create(boxed(storage), opts).value();
    Bytes before = content(10'000, 3);
    store.splice(0, 0, before).value();
    store.flush().value();
    // Small enough for the budget: no flush follows it.
    store.splice(100, 0, content(5'000, 4)).value();
    store.splice(0, 0, test::bytes("later")).value();
    EXPECT_EQ(store.stats().flushes, 1u);
    // Two full pages of the first splice, its remainder inline, and one of
    // the second, its remainder inline too.
    EXPECT_EQ(store.stats().payload_pages_written, 3u);
    // The unsynced writes are the second splice's payload page and then the
    // journal's appends; losing the page loses its transaction and all after
    // it.
    Bytes image = storage.crash_image([](std::size_t i) { return i != 0; });
    Store lost = Store::open(boxed(MemoryStorage::from_image(image)), opts).value();
    lost.check();
    EXPECT_EQ(lost.read_all().value(), before);
    Store kept = Store::open(boxed(MemoryStorage::from_image(
                                 storage.crash_image([](std::size_t) { return true; }))),
                             opts)
                     .value();
    kept.check();
    EXPECT_EQ(kept.size(), 15'005u);
}

TEST(Payload, ABitFlipInAPayloadPageDropsItsTransaction) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    Bytes before = content(10'000, 5);
    store.splice(0, 0, before).value();
    store.flush().value();
    Bytes later = content(5'000, 6);
    store.splice(0, 0, later).value();
    // The payload page is the data page whose content starts with the new
    // bytes; flip a bit in its content.
    Bytes image = storage.image();
    std::optional<std::size_t> page;
    for (std::size_t p = 0; p < image.size() / PAGE && !page; ++p) {
        std::size_t at = p * PAGE;
        if (image[at] == 0x02 && std::equal(later.begin(), later.begin() + 64, image.begin() + at + 11))
            page = p;
    }
    ASSERT_TRUE(page) << "the payload page is in the file";
    image[*page * PAGE + 11 + 10] ^= 1;
    Store reopened = Store::open(boxed(MemoryStorage::from_image(image))).value();
    EXPECT_EQ(reopened.read_all().value(), before);
}

/// A flush's header lost, and a large splice appended after it through the
/// seal: recovery replays both journals in one flush, which keeps the payload
/// pages of its own epoch where they lie and writes those of the next one
/// again.
TEST(Payload, PayloadPagesBeyondASealAreWrittenAgainByTheFlushThatFoldsThem) {
    Options opts = budget(16);
    MemoryStorage storage = MemoryStorage::with_crash_tracking();
    Store store = Store::create(boxed(storage), opts).value();
    for (std::uint8_t i = 0; i < 6; ++i) {
        store.splice(0, store.size(), content(3 * 4096, i)).value();
        store.flush().value();
    }
    store.splice(0, 0, content(6000, 10)).value();
    store.flush().value();
    Bytes late = content(7000, 11);
    store.splice(10, 0, late).value();
    Bytes expected = store.read_all().value();
    expected.insert(expected.begin() + 10, late.begin(), late.end());
    EXPECT_EQ(store.stats().flushes, 7u);
    // The first write after the last flush's `fsync` is its header.
    Bytes image = storage.crash_image([](std::size_t i) { return i != 0; });
    Store reopened = Store::open(boxed(MemoryStorage::from_image(image)), opts).value();
    reopened.check();
    EXPECT_EQ(reopened.read_all().value(), expected);
    Stats s = reopened.stats();
    EXPECT_GE(s.adopted_bytes, 6000u - 2048);
    EXPECT_GE(s.fresh_bytes, 7000u);
}

TEST(Payload, AThresholdOfUint32MaxKeepsEveryWriteInTheJournal) {
    Options opts;
    opts.payload_threshold = UINT32_MAX;
    Store store = Store::create(std::make_unique<MemoryStorage>(), opts).value();
    store.splice(0, 0, content(100'000, 9)).value();
    store.flush().value();
    Stats s = store.stats();
    EXPECT_EQ(s.payload_bytes, 0u);
    EXPECT_GT(s.journal_bytes, 100'000u);
}

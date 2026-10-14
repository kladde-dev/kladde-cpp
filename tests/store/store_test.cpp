// The store's and the storages' documented behaviour, case by case.
#include <gtest/gtest.h>

#include <filesystem>

#include <kladde/store.hpp>

#include "../../src/store/page.hpp"
#include "test_util.hpp"

using namespace kladde;
using test::boxed;
using test::bytes;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes read(Storage const& s, std::uint64_t offset, std::size_t n) {
    Bytes buf(n);
    s.read_at(buf, offset).value();
    return buf;
}

std::filesystem::path temp_path(char const* name) {
    return std::filesystem::temp_directory_path() / name;
}

}  // namespace

// ---------------------------------------------------------------- storages

TEST(MemoryStorage, ReadsWhatWasWritten) {
    MemoryStorage s;
    s.resize(8).value();
    s.write_at(bytes("kladde"), 1).value();
    s.sync().value();
    EXPECT_EQ(read(s, 0, 8), (Bytes{0, 'k', 'l', 'a', 'd', 'd', 'e', 0}));
    Bytes past(1);
    EXPECT_FALSE(s.read_at(past, 8));
}

TEST(MemoryStorage, ACrashImageKeepsAnySubsetOfTheWritesSinceTheLastSync) {
    MemoryStorage s = MemoryStorage::with_crash_tracking();
    s.resize(4).value();
    s.write_at(bytes("ab"), 0).value();
    s.sync().value();
    s.write_at(bytes("cd"), 2).value();
    EXPECT_EQ(s.crash_image([](std::size_t) { return false; }), (Bytes{'a', 'b', 0, 0}));
    EXPECT_EQ(s.crash_image([](std::size_t) { return true; }), bytes("abcd"));
}

TEST(MemoryStorage, CopiesShareTheImage) {
    MemoryStorage a;
    MemoryStorage b = a;
    a.resize(2).value();
    a.write_at(bytes("hi"), 0).value();
    EXPECT_EQ(b.image(), bytes("hi"));
    EXPECT_EQ(MemoryStorage::from_image(a.image()).size().value(), 2u);
}

TEST(MemoryStorage, InjectedFailures) {
    MemoryStorage s;
    s.fail_syncs();
    EXPECT_FALSE(s.sync());
    MemoryStorage t = MemoryStorage::with_crash_tracking();
    t.fail_after(1);
    EXPECT_TRUE(t.resize(4));
    EXPECT_FALSE(t.write_at(bytes("x"), 0));
    EXPECT_FALSE(t.sync());
}

TEST(FileStorage, WritesReachTheFileAndASecondOpenIsLockedOut) {
    auto path = temp_path("kladde-cpp-file-storage-test");
    {
        auto s = FileStorage::create(path).value();
        s->resize(4096).value();
        s->write_at(bytes("hello"), 0).value();
        s->sync().value();
        auto second = FileStorage::open(path);
        ASSERT_FALSE(second);
        EXPECT_EQ(second.error().code, errc::io);
    }
    auto s = FileStorage::open(path).value();
    EXPECT_EQ(s->size().value(), 4096u);
    EXPECT_EQ(read(*s, 0, 5), bytes("hello"));
    s.reset();
    std::filesystem::remove(path);
}

TEST(FileStorage, AStoreLivesInAFile) {
    auto path = temp_path("kladde-cpp-file-store-test");
    {
        Store store = Store::create(FileStorage::create(path).value()).value();
        store.splice(0, 0, Bytes(10'000, 3)).value();
        store.set_schema(bytes("types"));
        store.close().value();
    }
    {
        Store store = Store::open(FileStorage::open(path).value()).value();
        EXPECT_EQ(store.read_all().value(), Bytes(10'000, 3));
        EXPECT_EQ(store.schema(), bytes("types"));
        store.splice(5, 0, bytes("x")).value();
        // No flush: the journal keeps the splice.
    }
    Store store = Store::open(FileStorage::open(path).value()).value();
    EXPECT_EQ(store.size(), 10'001u);
    EXPECT_EQ(store.read_all().value()[5], 'x');
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------- opening

TEST(Store, OpeningRecoversTheJournal) {
    MemoryStorage storage;
    {
        Store store = Store::create(boxed(storage)).value();
        store.splice(0, 0, bytes("note")).value();
        // No flush: the process "dies" here, and the journal keeps the splice.
    }
    Store store = Store::open(boxed(MemoryStorage::from_image(storage.image()))).value();
    EXPECT_EQ(store.read_all().value(), bytes("note"));
    store.check();
}

TEST(Store, WhatIsNoKladdeFileIsRefused) {
    auto empty = Store::open(std::make_unique<MemoryStorage>());
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, errc::not_kladde);
    auto garbage = Store::open(boxed(MemoryStorage::from_image(Bytes(8192, 0x55))));
    ASSERT_FALSE(garbage);
    EXPECT_EQ(garbage.error().code, errc::not_kladde);
    // The magic, but neither header valid.
    MemoryStorage storage;
    Store::create(boxed(storage)).value();
    Bytes image = storage.image();
    image[60] ^= 1;
    auto broken = Store::open(boxed(MemoryStorage::from_image(image)));
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().code, errc::corrupt);
}

TEST(Store, ANewerFormatIsRefused) {
    MemoryStorage storage;
    Store::create(boxed(storage)).value();
    Bytes image = storage.image();
    // A newer header, of epoch 2 in slot 0, that needs reader version 1.
    store::HeaderFields h;
    h.min_reader_version = 1;
    h.journal_pointer = 2;
    store::PageBuf page{};
    std::uint8_t const empty[] = {0};
    store::encode_page(page, &h, store::KIND_TABLE, 2, empty);
    std::copy(page.begin(), page.end(), image.begin());
    auto r = Store::open(boxed(MemoryStorage::from_image(image)));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, errc::unsupported_version);
}

// ---------------------------------------------------------------- mutations

TEST(Store, WritesMovesAndBounds) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, bytes("hello")).value();
    store.write(0, bytes("j")).value();
    store.move(0, 1, 4).value();
    auto past = store.write(9, bytes("x"));
    ASSERT_FALSE(past);
    EXPECT_EQ(past.error().code, errc::out_of_bounds);
    EXPECT_FALSE(store.is_poisoned());
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("elloj"));
}

TEST(Store, CutsArePasted) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, bytes("abcdef")).value();
    CutId cut = *store.splice(1, 2, {}).value();
    EXPECT_EQ(cut.length, 2u);
    EXPECT_TRUE(store.is_alive(cut));
    store.paste(3, 0, cut, bytes("bc")).value();
    // A cut is pasted at most once.
    auto twice = store.paste(0, 0, cut, bytes("bc"));
    ASSERT_FALSE(twice);
    EXPECT_EQ(twice.error().code, errc::corrupt);
    auto wrong = store.paste(0, 0, CutId{cut.record, 3}, bytes("abc"));
    EXPECT_FALSE(wrong);
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("adebcf"));

    CutId old = *store.write(0, bytes("xy")).value();
    store.paste(6, 0, old, bytes("ad")).value();
    store.flush().value();
    EXPECT_EQ(store.read_all().value(), bytes("xyebcfad"));
}

TEST(Store, TransactionsKeepAllOrNothing) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    store.splice(0, 0, bytes("ab")).value();
    store.begin_transaction().value();
    store.write(0, bytes("x")).value();
    store.write(1, bytes("y")).value();
    // Not appended yet: a crash now keeps neither write.
    EXPECT_EQ(Store::open(boxed(MemoryStorage::from_image(storage.image()))).value().read_all().value(),
              bytes("ab"));
    store.end_transaction().value();
    EXPECT_EQ(Store::open(boxed(MemoryStorage::from_image(storage.image()))).value().read_all().value(),
              bytes("xy"));
    EXPECT_EQ(store.end_transaction().error().code, errc::corrupt);
    EXPECT_EQ(store.end_batch().error().code, errc::corrupt);
}

TEST(Store, AnAbandonedTransactionPoisons) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    store.splice(0, 0, bytes("a")).value();
    store.flush().value();
    store.begin_transaction().value();
    store.write(0, bytes("x")).value();
    store.abandon_transaction();
    EXPECT_EQ(store.flush().error().code, errc::poisoned);
    EXPECT_EQ(store.write(0, bytes("y")).error().code, errc::poisoned);
    Store reopened = Store::open(boxed(storage)).value();
    EXPECT_EQ(reopened.read_all().value(), bytes("a"));
}

TEST(Store, ABatchAppendsInFewPieces) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, Bytes(1000, 0)).value();
    store.begin_batch().value();
    for (std::uint64_t i = 0; i < 1000; ++i) {
        auto b = static_cast<std::uint8_t>(i);
        store.write(i, std::span(&b, 1)).value();
    }
    store.end_batch().value();
    EXPECT_EQ(store.stats().transactions, 2u);
}

TEST(Store, AFailedSyncPoisons) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    storage.fail_syncs();
    EXPECT_FALSE(store.flush());
    EXPECT_TRUE(store.is_poisoned());
    EXPECT_EQ(store.read_all().error().code, errc::poisoned);
}

// ---------------------------------------------------------------- flushing and reading

TEST(Store, CloseGivesSpaceBack) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    store.splice(0, 0, Bytes(100'000, 1)).value();
    store.flush().value();
    store.splice(0, 100'000, {}).value();
    store.flush().value();
    std::uint64_t before = storage.size().value();
    store.close().value();
    EXPECT_LT(storage.size().value(), before);
    EXPECT_TRUE(store.read_all().value().empty());
}

TEST(Store, FlushIfJournalExceeds) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, bytes("hi")).value();
    EXPECT_FALSE(store.flush_if_journal_exceeds(0.5).value());
    EXPECT_TRUE(store.flush_if_journal_exceeds(0.0).value());
    EXPECT_EQ(store.read_all().value(), bytes("hi"));
}

TEST(Store, TheSmallChainsAndTheFingerprintPersist) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    EXPECT_EQ(store.root_fingerprint(), (std::array<std::uint8_t, 16>{}));
    store.set_schema(bytes("types"));
    store.set_private_state(Bytes(300, 7));
    std::array<std::uint8_t, 16> fp;
    fp.fill(7);
    store.set_root_fingerprint(fp);
    EXPECT_EQ(store.schema(), bytes("types"));
    store.flush().value();
    Store reopened = Store::open(boxed(storage)).value();
    EXPECT_EQ(reopened.schema(), bytes("types"));
    EXPECT_EQ(reopened.private_state(), Bytes(300, 7));
    EXPECT_EQ(reopened.root_fingerprint(), fp);
    reopened.check();
}

TEST(Store, ReadingInChunksAndAfterEndLoad) {
    MemoryStorage storage;
    Store store = Store::create(boxed(storage)).value();
    Bytes content(10'000, 9);
    content.insert(content.end(), 5000, 0);  // a run of zeros
    store.splice(0, 0, content).value();
    store.flush().value();
    Store reopened = Store::open(boxed(storage)).value();
    reopened.end_load();
    std::size_t total = 0, chunks = 0;
    reopened.read([&](std::span<std::uint8_t const> c) {
        total += c.size();
        chunks += 1;
        return true;
    }).value();
    EXPECT_EQ(total, content.size());
    EXPECT_GT(chunks, 1u);
    std::size_t first_only = 0;
    reopened.read([&](std::span<std::uint8_t const> c) {
        first_only += c.size();
        return false;
    }).value();
    EXPECT_LT(first_only, content.size());
    EXPECT_EQ(reopened.read_all().value(), content);
    EXPECT_GT(reopened.stats().pages_read, 0u);
}

TEST(Store, StatsDescribeTheFile) {
    Store store = Store::create(std::make_unique<MemoryStorage>()).value();
    store.splice(0, 0, Bytes(20'000, 1)).value();
    store.flush().value();
    Stats s = store.stats();
    EXPECT_EQ(s.flushes, 1u);
    EXPECT_EQ(s.sequence_bytes, 20'000u);
    EXPECT_EQ(s.data_pages, 5u);
    EXPECT_GT(s.live_fraction(), 0.9);
}

TEST(Store, MovingAStoreKeepsIt) {
    Store a = Store::create(std::make_unique<MemoryStorage>()).value();
    a.splice(0, 0, bytes("moved")).value();
    Store b = std::move(a);
    b.flush().value();
    EXPECT_EQ(b.read_all().value(), bytes("moved"));
}

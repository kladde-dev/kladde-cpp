// Closing into a fully consolidated file keeps what the file reads, and
// leaves the smallest file that holds it: full data pages, one span per page
// and chain, and an empty journal.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include <kladde/store.hpp>

#include "test_util.hpp"

using namespace kladde;
using test::boxed;
using test::bytes;
using test::Rng;

namespace {

using Bytes = std::vector<std::uint8_t>;

/// Splices, writes and moves at random positions of a sequence that starts
/// with `size` bytes, `per_flush` operations between flushes; returns what
/// the sequence holds.
Bytes edit(Store& store, std::size_t size, int ops, int per_flush, std::uint64_t seed) {
    Rng rng{seed};
    Bytes model(size);
    for (std::size_t i = 0; i < size; ++i) model[i] = static_cast<std::uint8_t>(i % 251);
    store.splice(0, 0, model).value();
    for (int n = 0; n < ops; ++n) {
        std::uint64_t len = model.size();
        auto kind = rng.below(3);
        if (kind == 0) {
            std::uint64_t pos = rng.below(len + 1);
            std::uint64_t removed = rng.below(std::min<std::uint64_t>(len - pos, 64) + 1);
            Bytes b(rng.below(200));
            for (auto& x : b) x = static_cast<std::uint8_t>(rng.next());
            store.splice(pos, removed, b).value();
            model.erase(model.begin() + pos, model.begin() + pos + removed);
            model.insert(model.begin() + pos, b.begin(), b.end());
        } else if (kind == 1 && len > 0) {
            std::uint64_t pos = rng.below(len);
            Bytes b(std::min<std::uint64_t>(rng.below(len - pos) + 1, 300));
            for (auto& x : b) x = static_cast<std::uint8_t>(rng.next());
            store.write(pos, b).value();
            std::copy(b.begin(), b.end(), model.begin() + pos);
        } else if (len > 0) {
            std::uint64_t from = rng.below(len);
            std::uint64_t k = rng.below(std::min<std::uint64_t>(len - from, 5000)) + 1;
            std::uint64_t to = rng.below(len - k + 1);
            store.move(from, k, to).value();
            Bytes moved(model.begin() + from, model.begin() + from + k);
            model.erase(model.begin() + from, model.begin() + from + k);
            model.insert(model.begin() + to, moved.begin(), moved.end());
        }
        if (per_flush > 0 && n % per_flush == per_flush - 1) store.flush().value();
    }
    return model;
}

/// Checks that the file in `storage` reads `model`, `schema` and `priv`, and
/// is laid out as a fully consolidated file of them is; returns its stats.
Stats check_consolidated(MemoryStorage const& storage, Bytes const& model, Bytes const& schema,
                         Bytes const& priv) {
    auto store = Store::open(std::make_unique<MemoryStorage>(MemoryStorage::from_image(storage.image())))
                     .value();
    store.check();
    EXPECT_TRUE(store.read_all().value() == model) << "the sequence differs";
    EXPECT_EQ(store.schema(), schema);
    EXPECT_EQ(store.private_state(), priv);
    Stats s = store.stats();
    std::uint64_t total = model.size() + schema.size() + priv.size();
    std::uint64_t data_pages = (total + max_page_content - 1) / max_page_content;
    std::uint64_t chains = (model.empty() ? 0 : 1) + (schema.empty() ? 0 : 1) + (priv.empty() ? 0 : 1);
    EXPECT_EQ(s.data_pages, data_pages);
    EXPECT_EQ(s.live_data_bytes, total);
    // One span per page and chain: a page the end of one chain and the start
    // of the next share holds two.
    EXPECT_LE(s.spans, data_pages + (chains > 0 ? chains - 1 : 0));
    EXPECT_EQ(storage.size().value(), (2 + data_pages + s.table_pages) * page_size);
    return s;
}

}  // namespace

TEST(CloseConsolidated, AConsolidatedFileReadsAsTheStoreDid) {
    struct Case {
        std::size_t size;
        int ops, per_flush;
        std::uint64_t seed;
    };
    for (auto c : {Case{0, 0, 0, 1}, Case{10, 50, 0, 2}, Case{50'000, 3000, 200, 3},
                   Case{300'000, 2000, 0, 4}, Case{200'000, 5000, 50, 5}}) {
        MemoryStorage storage;
        auto store = Store::create(boxed(storage)).value();
        Bytes schema(c.size % 9000 + 3, 7);
        store.set_schema(schema);
        store.flush().value();
        Bytes model = edit(store, c.size, c.ops, c.per_flush, c.seed);
        Bytes priv = bytes("state");
        store.set_private_state(priv);
        std::array<std::uint8_t, 16> fingerprint;
        fingerprint.fill(static_cast<std::uint8_t>(c.size));
        store.set_root_fingerprint(fingerprint);
        store.close_consolidated().value();
        EXPECT_TRUE(store.is_poisoned());
        Stats s = check_consolidated(storage, model, schema, priv);
        EXPECT_EQ(s.table_pages, 0u) << "the header holds every entry";
        EXPECT_EQ(Store::open(boxed(storage)).value().root_fingerprint(), fingerprint);
    }
}

TEST(CloseConsolidated, ALargeFileKeepsItsEntriesInLeaves) {
    MemoryStorage storage;
    auto store = Store::create(boxed(storage)).value();
    Bytes model = edit(store, 5'000'000, 300, 100, 6);
    store.close_consolidated().value();
    EXPECT_GT(check_consolidated(storage, model, {}, {}).table_pages, 0u);
}

TEST(CloseConsolidated, TheFileTakesFurtherEditsAfterReopening) {
    MemoryStorage storage;
    Bytes model;
    {
        auto store = Store::create(boxed(storage)).value();
        model = edit(store, 20'000, 500, 100, 7);
        store.close_consolidated().value();
    }
    {
        auto store = Store::open(boxed(storage)).value();
        store.splice(100, 10, bytes("after")).value();
        model.erase(model.begin() + 100, model.begin() + 110);
        Bytes after = bytes("after");
        model.insert(model.begin() + 100, after.begin(), after.end());
    }
    auto store = Store::open(boxed(storage)).value();
    EXPECT_TRUE(store.read_all().value() == model);
    store.close_consolidated().value();
    check_consolidated(storage, model, {}, {});
}

TEST(CloseConsolidated, AnOpenTransactionIsDiscardedAndABatchKept) {
    MemoryStorage storage;
    auto store = Store::create(boxed(storage)).value();
    store.splice(0, 0, bytes("kept")).value();
    store.begin_batch().value();
    store.splice(4, 0, bytes(" too")).value();
    store.begin_transaction().value();
    store.splice(0, 0, bytes("lost")).value();
    store.close_consolidated().value();
    check_consolidated(storage, bytes("kept too"), {}, {});
}

TEST(CloseConsolidated, AFailureBeforeTheReplacementKeepsTheOldFile) {
    MemoryStorage storage = MemoryStorage::with_crash_tracking();
    auto store = Store::create(boxed(storage)).value();
    store.splice(0, 0, bytes("old")).value();
    store.flush().value();
    store.splice(3, 0, bytes(" and new")).value();
    storage.fail_after(0);
    EXPECT_FALSE(store.close_consolidated());
    // Every journal write made it, as they do but for a power cut.
    auto image = storage.crash_image([](std::size_t) { return true; });
    auto reopened = Store::open(std::make_unique<MemoryStorage>(MemoryStorage::from_image(image))).value();
    EXPECT_EQ(reopened.read_all().value(), bytes("old and new"));
}

TEST(CloseConsolidated, AFileIsReplacedInItsDirectory) {
    auto dir = std::filesystem::temp_directory_path() / "kladde-cpp-close-consolidated";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto path = dir / "drawing.kladde";
    auto store = Store::create(FileStorage::create(path).value()).value();
    Bytes model = edit(store, 100'000, 2000, 300, 8);
    auto before = std::filesystem::file_size(path);
    store.close_consolidated().value();
    std::vector<std::string> names;
    for (auto const& e : std::filesystem::directory_iterator(dir)) names.push_back(e.path().filename());
    EXPECT_EQ(names, std::vector<std::string>{"drawing.kladde"}) << "no temporary file is left";
    EXPECT_LT(std::filesystem::file_size(path), before);
    // The new file is locked as the old one was.
    EXPECT_FALSE(FileStorage::open(path));
    std::ifstream in(path, std::ios::binary);
    Bytes image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    check_consolidated(MemoryStorage::from_image(image), model, {}, {});
    store = Store::create(std::make_unique<MemoryStorage>()).value();
    std::filesystem::remove_all(dir);
}

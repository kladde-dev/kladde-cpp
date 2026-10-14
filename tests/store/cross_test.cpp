// Files for cross-implementation tests: with KLADDE_CROSS_DIR set, writes
// cpp-SEED.kladde files there, each with many flushes, consolidation and an
// unflushed journal, beside cpp-SEED.expected, what kladde-store-dump (or
// another implementation's equivalent) must print for it. Skipped otherwise.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <kladde/store.hpp>

#include "test_util.hpp"

using namespace kladde;
using test::bytes;

namespace {

using Bytes = std::vector<std::uint8_t>;

std::string hex(std::span<std::uint8_t const> b) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    for (std::uint8_t x : b) {
        s.push_back(digits[x >> 4]);
        s.push_back(digits[x & 15]);
    }
    return s;
}

void edits(Store& store, Bytes& model, test::Rng& rng, int steps, std::uint64_t flush_every) {
    auto splice_model = [&](std::uint64_t pos, std::uint64_t removed, Bytes const& with) {
        Bytes gone(model.begin() + static_cast<std::ptrdiff_t>(pos),
                   model.begin() + static_cast<std::ptrdiff_t>(pos + removed));
        model.erase(model.begin() + static_cast<std::ptrdiff_t>(pos),
                    model.begin() + static_cast<std::ptrdiff_t>(pos + removed));
        model.insert(model.begin() + static_cast<std::ptrdiff_t>(pos), with.begin(), with.end());
        return gone;
    };
    for (int i = 0; i < steps; ++i) {
        std::uint64_t len = model.size();
        std::uint64_t kind = rng.below(6);
        if (kind <= 1) {
            std::uint64_t pos = rng.below(len + 1);
            std::uint64_t removed = rng.below(std::min<std::uint64_t>(len - pos, 50) + 1);
            std::uint64_t n = rng.below(10) == 0 ? rng.below(9000) : rng.below(30);
            auto fill = static_cast<std::uint8_t>(rng.below(250) + 1);
            Bytes b(n);
            for (std::uint64_t k = 0; k < n; ++k) b[k] = static_cast<std::uint8_t>(fill + k);
            splice_model(pos, removed, b);
            store.splice(pos, removed, b).value();
        } else if (kind == 2 && len > 0) {
            std::uint64_t pos = rng.below(len);
            std::uint64_t n = rng.below(std::min<std::uint64_t>(len - pos, 300)) + 1;
            Bytes b(n, static_cast<std::uint8_t>(rng.below(250) + 1));
            splice_model(pos, n, b);
            store.write(pos, b).value();
        } else if (kind == 3 && len > 0) {
            std::uint64_t from = rng.below(len);
            std::uint64_t n = rng.below(len - from) + 1;
            std::uint64_t to = rng.below(len - n + 1);
            Bytes m = splice_model(from, n, {});
            splice_model(to, 0, m);
            store.move(from, n, to).value();
        } else if (kind == 4 && len > 1) {
            std::uint64_t a = rng.below(len);
            std::uint64_t n = rng.below(std::min<std::uint64_t>(len - a, 500)) + 1;
            Bytes cut = splice_model(a, n, {});
            std::uint64_t to = rng.below(model.size() + 1);
            splice_model(to, 0, cut);
            store.atomically([&]() -> expected<void> {
                     KLADDE_TRY_ASSIGN(auto c, store.splice(a, n, {}));
                     return store.paste(to, 0, *c, cut);
                 })
                .value();
        } else {
            Bytes b(rng.below(200), 0);
            std::uint64_t pos = rng.below(len + 1);
            splice_model(pos, 0, b);
            store.splice(pos, 0, b).value();
        }
        if (flush_every > 0 && std::uint64_t(i) % flush_every == flush_every - 1) store.flush().value();
    }
}

}  // namespace

TEST(Cross, WritesFilesForAnotherImplementation) {
    char const* dir = std::getenv("KLADDE_CROSS_DIR");
    if (!dir) GTEST_SKIP() << "KLADDE_CROSS_DIR is not set";
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        auto path = std::filesystem::path(dir) / ("cpp-" + std::to_string(seed) + ".kladde");
        test::Rng rng{(seed * 0x9E3779B97F4A7C15) | 1};
        Store store = Store::create(FileStorage::create(path).value()).value();
        Bytes model;
        Bytes schema;
        for (int k = 0; k < 40; ++k) schema.insert(schema.end(), {'a', ' ', 's', 'c', 'h', 'e', 'm', 'a'});
        store.set_schema(schema);
        std::array<std::uint8_t, 16> fp;
        fp.fill(static_cast<std::uint8_t>(seed));
        store.set_root_fingerprint(fp);
        edits(store, model, rng, 600, 23);
        store.flush().value();
        // An unflushed journal, which the reader must recover.
        edits(store, model, rng, 60, 0);
        std::ofstream out(std::filesystem::path(dir) / ("cpp-" + std::to_string(seed) + ".expected"));
        out << "sequence " << hex(model) << "\nroot_fingerprint " << hex(fp) << "\nschema " << hex(schema)
            << "\n";
    }
    // Files whose last header write was lost after more was appended: a
    // reader must follow the seal to find the appended transactions.
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        std::string name = "cpp-lost-header-" + std::to_string(seed);
        test::Rng rng{seed | 1};
        MemoryStorage storage = MemoryStorage::with_crash_tracking();
        Store store = Store::create(test::boxed(storage)).value();
        Bytes model;
        edits(store, model, rng, 300, 29);
        store.flush().value();
        edits(store, model, rng, 40, 0);
        Bytes image = storage.crash_image([](std::size_t i) { return i != 0; });
        std::ofstream(std::filesystem::path(dir) / (name + ".kladde"), std::ios::binary)
            .write(reinterpret_cast<char const*>(image.data()), static_cast<std::streamsize>(image.size()));
        std::ofstream out(std::filesystem::path(dir) / (name + ".expected"));
        out << "sequence " << hex(model) << "\nroot_fingerprint " << hex(std::array<std::uint8_t, 16>{})
            << "\nschema \n";
    }
}

// Prints a kladde file's schema, one descriptor per line, and its root
// fingerprint: for comparing what two implementations wrote.
//
//     kladde-schema-dump drawing.kladde
#include <cstdio>

#include <kladde/schema.hpp>
#include <kladde/store.hpp>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: kladde-schema-dump <file>\n");
        return 2;
    }
    // Opening folds a journal a crashed writer left; read a copy to leave
    // the file as it is.
    auto storage = kladde::FileStorage::open(argv[1]);
    if (!storage) {
        std::fprintf(stderr, "%s\n", storage.error().what().c_str());
        return 1;
    }
    auto size = (*storage)->size().value();
    std::vector<std::uint8_t> image(size);
    (void)(*storage)->read_at(image, 0).value();
    auto store = kladde::Store::open(
        std::make_unique<kladde::MemoryStorage>(kladde::MemoryStorage::from_image(std::move(image))));
    if (!store) {
        std::fprintf(stderr, "%s\n", store.error().what().c_str());
        return 1;
    }
    auto table = kladde::schema::TypeTable::decode(store->schema());
    if (!table) {
        std::fprintf(stderr, "%s\n", table.error().what().c_str());
        return 1;
    }
    std::printf("%s", kladde::schema::dump(*table).c_str());
    std::printf("fingerprint %s (recorded %s)\n", table->fingerprint().to_hex().c_str(),
                kladde::to_hex(store->root_fingerprint()).c_str());
    return 0;
}

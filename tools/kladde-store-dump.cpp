// Prints what a kladde file holds, for comparing implementations: its
// sequence, its root fingerprint and its schema chain, each as lowercase hex
// on a line of its own.
//
//     kladde-store-dump FILE
//
// Opening recovers and folds a journal that a crashed writer left, as any
// open does, so the file may change. Exits with 1 if the file does not open.
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include <kladde/store.hpp>

namespace {

std::string hex(std::span<std::uint8_t const> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s FILE\n", argv[0]);
        return 2;
    }
    auto storage = kladde::FileStorage::open(argv[1]);
    if (!storage) {
        std::fprintf(stderr, "%s: %s\n", argv[1], storage.error().what().c_str());
        return 1;
    }
    auto store = kladde::Store::open(std::move(*storage));
    if (!store) {
        std::fprintf(stderr, "%s: %s\n", argv[1], store.error().what().c_str());
        return 1;
    }
    auto sequence = store->read_all();
    if (!sequence) {
        std::fprintf(stderr, "%s: %s\n", argv[1], sequence.error().what().c_str());
        return 1;
    }
    auto fingerprint = store->root_fingerprint();
    std::printf("sequence %s\n", hex(*sequence).c_str());
    std::printf("root_fingerprint %s\n", hex(fingerprint).c_str());
    std::printf("schema %s\n", hex(store->schema()).c_str());
    return 0;
}

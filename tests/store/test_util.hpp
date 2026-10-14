// Helpers the store's tests share.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <kladde/store.hpp>

namespace kladde::test {

/// The xorshift generator kladde-rs's tests use, so that seeds mean the same.
struct Rng {
    std::uint64_t state;

    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    std::uint64_t below(std::uint64_t n) { return next() % (n == 0 ? 1 : n); }
};

inline std::vector<std::uint8_t> bytes(std::string_view s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

inline std::unique_ptr<MemoryStorage> boxed(MemoryStorage const& s) {
    return std::make_unique<MemoryStorage>(s);
}

}  // namespace kladde::test

#include "crc.hpp"

#include <array>

#include "util.hpp"

namespace kladde::store {

namespace {

constexpr std::uint32_t POLY = 0x82F63B78;

using Tables = std::array<std::array<std::uint32_t, 256>, 8>;

constexpr Tables make_tables() {
    Tables t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ POLY : c >> 1;
        t[0][i] = c;
    }
    for (std::size_t i = 0; i < 256; ++i)
        for (std::size_t s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xff];
    return t;
}

constexpr Tables TABLES = make_tables();

}  // namespace

void Crc32c::update(std::span<std::uint8_t const> bytes) {
    auto const& t = TABLES;
    std::uint32_t crc = state_;
    std::size_t i = 0;
    for (; i + 8 <= bytes.size(); i += 8) {
        std::uint8_t const* c = bytes.data() + i;
        std::uint32_t lo = load_u32(c) ^ crc;
        std::uint32_t hi = load_u32(c + 4);
        crc = t[7][lo & 0xff] ^ t[6][(lo >> 8) & 0xff] ^ t[5][(lo >> 16) & 0xff] ^ t[4][lo >> 24] ^
              t[3][hi & 0xff] ^ t[2][(hi >> 8) & 0xff] ^ t[1][(hi >> 16) & 0xff] ^ t[0][hi >> 24];
    }
    for (; i < bytes.size(); ++i) crc = t[0][(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    state_ = crc;
}

std::uint32_t crc32c(std::span<std::uint8_t const> bytes) {
    Crc32c c;
    c.update(bytes);
    return c.value();
}

}  // namespace kladde::store

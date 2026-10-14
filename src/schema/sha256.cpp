// SHA-256 as FIPS 180-4 defines it, small rather than fast: fingerprinting
// hashes a few hundred bytes per schema.
#include <kladde/schema.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace kladde::schema {

namespace {

constexpr std::array<std::uint32_t, 8> initial = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::array<std::uint32_t, 64> round_constants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

void compress(std::array<std::uint32_t, 8>& h, std::uint8_t const* block) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = std::uint32_t(block[4 * i]) << 24 | std::uint32_t(block[4 * i + 1]) << 16 |
               std::uint32_t(block[4 * i + 2]) << 8 | std::uint32_t(block[4 * i + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, hh] = h;
    for (std::size_t i = 0; i < 64; ++i) {
        auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        auto ch = (e & f) ^ (~e & g);
        auto t1 = hh + s1 + ch + round_constants[i] + w[i];
        auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        auto maj = (a & b) ^ (a & c) ^ (b & c);
        auto t2 = s0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // namespace

Sha256::Sha256() : h_(initial) {}

void Sha256::update(std::span<std::uint8_t const> data) {
    total_ += data.size();
    while (!data.empty()) {
        std::size_t n = std::min(data.size(), block_.size() - used_);
        std::memcpy(block_.data() + used_, data.data(), n);
        used_ += n;
        data = data.subspan(n);
        if (used_ == block_.size()) {
            compress(h_, block_.data());
            used_ = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::finalize() {
    // The padding: 0x80, zeros up to 56 bytes into a block, then the
    // message's length in bits, big-endian.
    std::uint64_t bits = total_ * 8;
    block_[used_++] = 0x80;
    if (used_ > 56) {
        std::fill(block_.begin() + std::ptrdiff_t(used_), block_.end(), std::uint8_t{0});
        compress(h_, block_.data());
        used_ = 0;
    }
    std::fill(block_.begin() + std::ptrdiff_t(used_), block_.begin() + 56, std::uint8_t{0});
    for (std::size_t i = 0; i < 8; ++i) block_[56 + i] = std::uint8_t(bits >> (56 - 8 * i));
    compress(h_, block_.data());

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        for (std::size_t j = 0; j < 4; ++j) digest[4 * i + j] = std::uint8_t(h_[i] >> (24 - 8 * j));
    }
    *this = Sha256();
    return digest;
}

std::array<std::uint8_t, 32> sha256(std::span<std::uint8_t const> data) {
    Sha256 h;
    h.update(data);
    return h.finalize();
}

}  // namespace kladde::schema

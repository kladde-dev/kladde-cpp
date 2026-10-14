// LEB128 varints, as the canonical encoding and the value encodings use them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace kladde {

/// How many bytes the varint of `v` takes: 1 to 10.
constexpr std::size_t varint_len(std::uint64_t v) noexcept {
    std::size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

/// Appends the varint of `v` to `out`.
inline void put_varint(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

/// Reads a varint from the front of `in`, advancing it; `nullopt` if `in`
/// ends inside it, or if it is longer than 10 bytes or overflows 64 bits.
/// With `minimal`, also if it is not the shortest encoding of its value.
inline std::optional<std::uint64_t> get_varint(std::span<std::uint8_t const>& in,
                                               bool minimal = false) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 10; ++i) {
        if (i >= in.size()) return std::nullopt;
        std::uint8_t b = in[i];
        if (i == 9 && b > 1) return std::nullopt;
        v |= std::uint64_t(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            if (minimal && i > 0 && b == 0) return std::nullopt;
            in = in.subspan(i + 1);
            return v;
        }
    }
    return std::nullopt;
}

/// Zigzag mapping of signed integers onto unsigned ones, small magnitudes
/// first.
constexpr std::uint64_t zigzag(std::int64_t v) noexcept {
    return (static_cast<std::uint64_t>(v) << 1) ^ static_cast<std::uint64_t>(v >> 63);
}
constexpr std::int64_t unzigzag(std::uint64_t v) noexcept {
    return static_cast<std::int64_t>(v >> 1) ^ -static_cast<std::int64_t>(v & 1);
}

}  // namespace kladde

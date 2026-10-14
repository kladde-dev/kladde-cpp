// CRC-32C (Castagnoli), as spec/file-format.md specifies it: reflected
// polynomial 0x82F63B78, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF.
// Slicing-by-8, which is fast enough that the checksum never shows next to
// the I/O it guards.
#pragma once

#include <cstdint>
#include <span>

namespace kladde::store {

/// A running CRC-32C.
///
/// `value()` is the CRC of everything fed so far, and feeding more afterwards
/// continues the same computation, which is what the journal's chain needs:
/// every checksum in the journal is a value of one running CRC.
class Crc32c {
public:
    constexpr Crc32c() = default;

    /// Feeds `bytes`.
    void update(std::span<std::uint8_t const> bytes);

    /// The CRC-32C of everything fed so far.
    std::uint32_t value() const { return ~state_; }

private:
    std::uint32_t state_ = ~std::uint32_t{0};
};

/// The CRC-32C of `bytes`.
std::uint32_t crc32c(std::span<std::uint8_t const> bytes);

}  // namespace kladde::store

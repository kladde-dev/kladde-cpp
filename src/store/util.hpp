// Small helpers every module of the store shares: error constructors,
// invariant checks, little-endian fields, and varints.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <kladde/error.hpp>
#include <kladde/varint.hpp>

namespace kladde::store {

/// A failure for a file that violates the format.
inline error corrupt(std::string msg) { return error{errc::corrupt, std::move(msg)}; }

/// A failure of reading or writing the file.
inline error io_error(std::string msg) { return error{errc::io, std::move(msg)}; }

/// Prints `msg` with where it failed and aborts: a broken invariant.
[[noreturn]] void fail_check(char const* msg, char const* file, int line);

}  // namespace kladde::store

/// Checks an invariant in every build: a violated one is a bug in the store.
#define KLADDE_CHECK(cond, msg)                                       \
    do {                                                              \
        if (!(cond)) ::kladde::store::fail_check(msg, __FILE__, __LINE__); \
    } while (false)

/// Checks an invariant in debug builds only.
#ifdef NDEBUG
#define KLADDE_DEBUG_CHECK(cond, msg) \
    do {                              \
    } while (false)
#else
#define KLADDE_DEBUG_CHECK(cond, msg) KLADDE_CHECK(cond, msg)
#endif

namespace kladde::store {

inline std::uint16_t load_u16(std::uint8_t const* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::uint32_t load_u32(std::uint8_t const* p) {
    return std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 |
           std::uint32_t{p[3]} << 24;
}

inline std::uint64_t load_u64(std::uint8_t const* p) {
    return std::uint64_t{load_u32(p)} | std::uint64_t{load_u32(p + 4)} << 32;
}

inline void store_u16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

inline void store_u32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline void store_u64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline std::array<std::uint8_t, 4> le_bytes(std::uint32_t v) {
    std::array<std::uint8_t, 4> out{};
    store_u32(out.data(), v);
    return out;
}

inline std::array<std::uint8_t, 8> le_bytes(std::uint64_t v) {
    std::array<std::uint8_t, 8> out{};
    store_u64(out.data(), v);
    return out;
}

/// Zigzag of a signed difference, as the journal and the span table write
/// relative positions.
inline std::uint64_t zz(std::int64_t v) { return zigzag(v); }

}  // namespace kladde::store

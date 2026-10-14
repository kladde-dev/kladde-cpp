// A value over a `VectorBackend`: every mutation applied to a byte vector at
// once, which must then equal the value's encoding.
#pragma once

#include <gtest/gtest.h>

#include <kladde/kladde.hpp>

namespace kladde::testing {

template <class T>
struct Fixture final : Anchor {
    explicit Fixture(T v) : value(std::move(v)), bytes(to_bytes(value)) { attach(value, this); }
    T value;
    mutable VectorBackend bytes;

    Mut<T> edit() { return Mut<T>(value, Region(bytes, Link::at_anchor(this, bytes.size()), this)); }

    /// The bytes recorded equal the value's encoding, which decodes to the
    /// value.
    void check() const {
        ASSERT_EQ(bytes.bytes(), to_bytes(value));
        auto decoded = from_bytes<T>(bytes.bytes());
        ASSERT_TRUE(decoded.has_value()) << decoded.error().what();
        ASSERT_TRUE(*decoded == value);
    }

    std::uint64_t start() const override { return 0; }
    void grew(std::int64_t) override {}
    Backend& backend() const override { return bytes; }
    bool locate(void const* target, std::uint64_t& offset) const override {
        if (static_cast<void const*>(&value) == target) return true;
        return kladde::locate(value, target, offset);
    }
};

}  // namespace kladde::testing

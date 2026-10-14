#include <kladde/schema.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "helpers.hpp"

using namespace kladde::schema;
using kladde::schema::test::bytes;

namespace {

std::string hex(std::array<std::uint8_t, 32> const& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    for (auto b : digest) {
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

std::string sha256_hex(std::string const& message) { return hex(sha256(bytes(message))); }

}  // namespace

TEST(Sha256, MatchesKnownVectors) {
    EXPECT_EQ(sha256_hex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(sha256_hex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 55 bytes: the length fits the same block, without zero padding.
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnop"),
              "aa353e009edbaebfc6e494c8d847696896cb8b398e0173a4b5c1b636292d87c7");
    // 56 bytes: the length needs an extra block.
    EXPECT_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // 64 bytes: exactly one block.
    EXPECT_EQ(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"),
              "2ff100b36c386c65a1afc462ad53e25479bec9498ed00aa5a04de584bc25301b");
    // 112 bytes: longer than one block.
    EXPECT_EQ(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmno"
                         "pqrlmnopqrsmnopqrstnopqrstu"),
              "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

TEST(Sha256, StreamingMatchesOneShot) {
    std::vector<std::uint8_t> message;
    for (std::size_t i = 0; i < 1000; ++i) message.push_back(std::uint8_t(i % 251));
    auto one_shot = sha256(message);

    for (std::size_t chunk : {1, 7, 63, 64, 65, 128}) {
        Sha256 h;
        for (std::size_t at = 0; at < message.size(); at += chunk) {
            h.update(std::span(message).subspan(at, std::min(chunk, message.size() - at)));
        }
        EXPECT_EQ(h.finalize(), one_shot) << "chunk size " << chunk;
    }

    Sha256 h;
    for (auto b : message) h.update(b);
    EXPECT_EQ(h.finalize(), one_shot);
}

TEST(Sha256, StartsOverAfterFinalizing) {
    Sha256 h;
    h.update(bytes("xyz"));
    (void)h.finalize();
    h.update(bytes("abc"));
    EXPECT_EQ(h.finalize(), sha256(bytes("abc")));
}

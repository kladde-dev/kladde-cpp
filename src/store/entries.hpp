// The encoding of a span-table page's content: child references, then
// entries sorted by span id.
//
//     table_page := child_ref* 0:byte entry*
//     entry      := id_delta:varint tag:byte body next?
//     ; bit 0 of tag: 1 if the next span is id + 1, and no `next` field follows
//     ; tag >> 1 = 0:      Ref     body := length:varint address_delta:varint
//     ; tag >> 1 = 1:      Zero    body := length:varint
//     ; tag >> 1 = 2..127: Inline  body := payload:byte{(tag >> 1) - 1}
//     next       := varint ; 0: the chain ends here; else zigzag(next - id)
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <kladde/error.hpp>

namespace kladde::store {

/// What an entry says a span shows: `len` bytes at file address `addr`, or
/// `len` zeros, or an inline `payload`.
struct Body {
    enum class Kind : std::uint8_t { ref, zero, inline_ } kind = Kind::ref;
    std::uint32_t len = 0;
    std::uint64_t addr = 0;
    std::span<std::uint8_t const> payload;

    static Body ref(std::uint32_t len, std::uint64_t addr) { return {Kind::ref, len, addr, {}}; }
    static Body zero(std::uint32_t len) { return {Kind::zero, len, 0, {}}; }
    static Body inline_(std::span<std::uint8_t const> p) {
        return {Kind::inline_, static_cast<std::uint32_t>(p.size()), 0, p};
    }
};

/// An entry to encode.
struct EntryOut {
    std::uint32_t id = 0;
    std::uint32_t next = 0;
    Body body;
};

/// Where an encoded entry landed in the content.
struct Placed {
    std::uint32_t id = 0;
    /// The bytes the entry takes, payload included.
    std::uint16_t len = 0;
    /// Where an `Inline` entry's payload starts within the content.
    std::size_t payload = 0;
};

/// The bytes the child references take, the delimiter included.
std::size_t children_len(std::span<std::uint32_t const> children);

/// Encodes child references, in ascending order, then entries, sorted by id,
/// into `out`; returns where each entry landed.
std::vector<Placed> encode_entries(std::span<std::uint32_t const> children,
                                   std::span<EntryOut const> entries, std::vector<std::uint8_t>& out);

/// Encodes entries one at a time into a page's content, refusing one that
/// would not fit.
class PageEncoder {
public:
    /// A page whose content, child references and delimiter included, may
    /// take `capacity` bytes.
    PageEncoder(std::span<std::uint32_t const> children, std::size_t capacity);
    /// Appends `e`, whose id must exceed every id before it, if it fits.
    bool push(EntryOut const& e);

    std::vector<std::uint8_t> out;
    std::vector<Placed> placed;

private:
    std::size_t capacity_;
    std::uint32_t prev_id_ = 0;
    std::uint64_t prev_end_ = 0;
};

/// About the bytes an entry takes among neighbours of nearby ids, for
/// choosing what goes where before anything is encoded.
std::size_t estimate(std::uint32_t id, std::uint32_t next, Body const& body);

/// A decoded entry: a `Ref` of `len` bytes at `addr`, a `Zero` of `len`, or
/// an `Inline` payload of `len` bytes starting `offset` bytes into the
/// content.
struct EntryIn {
    std::uint32_t id = 0;
    std::uint32_t next = 0;
    Body::Kind kind = Body::Kind::ref;
    std::uint32_t len = 0;
    std::uint64_t addr = 0;
    std::size_t offset = 0;
    /// The bytes the entry takes, payload included.
    std::uint16_t entry_len = 0;
};

/// A decoded page content.
struct Decoded {
    std::vector<std::uint32_t> children;
    std::vector<EntryIn> entries;
};

/// Decodes a table page's content, checking what the format requires of each
/// entry on its own.
expected<Decoded> decode_entries(std::span<std::uint8_t const> content);

}  // namespace kladde::store

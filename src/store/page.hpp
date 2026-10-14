// Page framing and the header fields (spec/file-format.md).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

#include "consts.hpp"

namespace kladde::store {

/// A page-sized buffer.
using PageBuf = std::array<std::uint8_t, PAGE_SIZE>;

/// A fresh, zeroed page buffer on the heap.
inline std::unique_ptr<PageBuf> new_page() { return std::make_unique<PageBuf>(); }

/// The fields of a header page's `header`.
struct HeaderFields {
    std::uint16_t format_version = 0;
    std::uint16_t min_reader_version = 0;
    /// The first span of the sequence's chain, or 0 for an empty sequence.
    std::uint32_t sequence_head = 0;
    /// The first span of the schema's chain, or 0.
    std::uint32_t schema_head = 0;
    /// The first span of the implementation's private chain, or 0.
    std::uint32_t private_head = 0;
    std::array<std::uint8_t, 16> root_fingerprint{};
    std::uint32_t journal_pointer = 0;

    bool operator==(HeaderFields const&) const = default;
};

/// Why a page did not decode.
enum class PageError {
    /// The CRC does not match, or `content_size` leaves no room for it.
    bad_checksum,
    /// A header page without the kladde magic.
    not_kladde,
    /// A header page declaring a page size this implementation cannot read.
    unsupported_page_size,
};

/// A page that passed its checks.
struct DecodedPage {
    std::optional<HeaderFields> header;
    std::uint8_t kind = 0;
    std::uint64_t epoch = 0;
    /// Where `content` lies within the page: `[content_start, content_end)`.
    std::size_t content_start = 0;
    std::size_t content_end = 0;
};

/// Frames `content` into `buf` as a complete page, padding with zeros.
/// `header` is non-null for a header page. Aborts if `content` does not fit.
void encode_page(PageBuf& buf, HeaderFields const* header, std::uint8_t kind, std::uint64_t epoch,
                 std::span<std::uint8_t const> content);

/// Frames a page whose `content` is already in place at its offset: writes
/// `kind`, `epoch`, `content_size`, the CRC, and zero padding.
void seal_page(PageBuf& buf, HeaderFields const* header, std::uint8_t kind, std::uint64_t epoch,
               std::size_t len);

/// Checks a page's framing and decodes it.
std::variant<DecodedPage, PageError> decode_page(PageBuf const& buf, bool is_header);

}  // namespace kladde::store

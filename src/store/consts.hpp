// Format constants from spec/file-format.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kladde::store {

/// Binary logarithm of the page size. Only 4 KiB pages are allowed.
inline constexpr std::uint8_t LOG2_PAGE_SIZE = 12;
/// The page size in bytes.
inline constexpr std::size_t PAGE_SIZE = std::size_t{1} << LOG2_PAGE_SIZE;
/// `kind` (1) + `epoch` (8) + `content_size` (2) + `crc` (4).
inline constexpr std::size_t FRAMING = 15;
/// The largest `content` a non-header page holds.
inline constexpr std::size_t MAX_PAGE_CONTENT = PAGE_SIZE - FRAMING;
/// The width of the `header` field of a header page.
inline constexpr std::size_t HEADER_FIELDS = 48;
/// The largest `content` a header page holds.
inline constexpr std::size_t MAX_HEADER_CONTENT = MAX_PAGE_CONTENT - HEADER_FIELDS;
/// Where `content` starts in a non-header page: after `kind`, `epoch`, and
/// `content_size`.
inline constexpr std::size_t CONTENT_OFFSET = 11;
/// Where `content` starts in a header page.
inline constexpr std::size_t HEADER_CONTENT_OFFSET = HEADER_FIELDS + CONTENT_OFFSET;
/// The bytes of a journal page that carry the transaction stream; the last 8
/// are the trailer.
inline constexpr std::size_t JOURNAL_STREAM = PAGE_SIZE - 8;

/// `kind` of a span-table page, including the header pages.
inline constexpr std::uint8_t KIND_TABLE = 0x01;
/// `kind` of a data page.
inline constexpr std::uint8_t KIND_DATA = 0x02;

/// The file magic, `\x8B K L A D D E \r \n \x1A \n`.
inline constexpr std::array<std::uint8_t, 11> MAGIC = {0x8B, 0x4B, 0x4C, 0x41, 0x44, 0x44,
                                                       0x45, 0x0D, 0x0A, 0x1A, 0x0A};
/// The specification version this implementation writes.
inline constexpr std::uint16_t FORMAT_VERSION = 0;
/// The oldest reader version that can read what this implementation writes.
inline constexpr std::uint16_t MIN_READER_VERSION = 0;
/// The newest `min_reader_version` this implementation can read.
inline constexpr std::uint16_t READER_VERSION = 0;

/// The format's ceiling on an `Inline` entry's payload.
inline constexpr std::size_t MAX_INLINE = 126;
/// The largest a sequence may be: positions stay below `2^48`.
inline constexpr std::uint64_t MAX_SEQUENCE = std::uint64_t{1} << 48;

/// No page.
inline constexpr std::uint32_t NONE = UINT32_MAX;

}  // namespace kladde::store

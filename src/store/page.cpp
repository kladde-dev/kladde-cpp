#include "page.hpp"

#include <algorithm>
#include <cstring>

#include "crc.hpp"
#include "util.hpp"

namespace kladde::store {

namespace {

void encode_header(HeaderFields const& h, std::uint8_t* out) {
    std::memcpy(out, MAGIC.data(), MAGIC.size());
    out[11] = LOG2_PAGE_SIZE;
    store_u16(out + 12, h.format_version);
    store_u16(out + 14, h.min_reader_version);
    store_u32(out + 16, h.sequence_head);
    store_u32(out + 20, h.schema_head);
    store_u32(out + 24, h.private_head);
    std::memcpy(out + 28, h.root_fingerprint.data(), 16);
    store_u32(out + 44, h.journal_pointer);
}

std::variant<HeaderFields, PageError> decode_header(std::uint8_t const* bytes) {
    if (std::memcmp(bytes, MAGIC.data(), MAGIC.size()) != 0) return PageError::not_kladde;
    if (bytes[11] != LOG2_PAGE_SIZE) return PageError::unsupported_page_size;
    HeaderFields h;
    h.format_version = load_u16(bytes + 12);
    h.min_reader_version = load_u16(bytes + 14);
    h.sequence_head = load_u32(bytes + 16);
    h.schema_head = load_u32(bytes + 20);
    h.private_head = load_u32(bytes + 24);
    std::memcpy(h.root_fingerprint.data(), bytes + 28, 16);
    h.journal_pointer = load_u32(bytes + 44);
    return h;
}

/// Where `kind` sits in a header page or an ordinary one.
std::size_t kind_offset(bool is_header) { return is_header ? HEADER_FIELDS : 0; }

/// The CRC over everything before `content_size`, then the content.
std::uint32_t frame_crc(PageBuf const& buf, std::size_t k, std::size_t len) {
    Crc32c c;
    c.update(std::span(buf.data(), k + 9));
    std::size_t start = k + CONTENT_OFFSET;
    c.update(std::span(buf.data() + start, len));
    return c.value();
}

}  // namespace

void seal_page(PageBuf& buf, HeaderFields const* header, std::uint8_t kind, std::uint64_t epoch,
               std::size_t len) {
    std::size_t k = kind_offset(header != nullptr);
    if (header) encode_header(*header, buf.data());
    std::size_t start = k + CONTENT_OFFSET;
    KLADDE_CHECK(start + len + 4 <= PAGE_SIZE, "page content does not fit");
    buf[k] = kind;
    store_u64(buf.data() + k + 1, epoch);
    store_u16(buf.data() + k + 9, static_cast<std::uint16_t>(len));
    std::uint32_t crc = frame_crc(buf, k, len);
    std::size_t end = start + len;
    store_u32(buf.data() + end, crc);
    std::fill(buf.begin() + static_cast<std::ptrdiff_t>(end + 4), buf.end(), 0);
}

void encode_page(PageBuf& buf, HeaderFields const* header, std::uint8_t kind, std::uint64_t epoch,
                 std::span<std::uint8_t const> content) {
    std::size_t start = kind_offset(header != nullptr) + CONTENT_OFFSET;
    KLADDE_CHECK(start + content.size() + 4 <= PAGE_SIZE, "page content does not fit");
    if (!content.empty()) std::memmove(buf.data() + start, content.data(), content.size());
    seal_page(buf, header, kind, epoch, content.size());
}

std::variant<DecodedPage, PageError> decode_page(PageBuf const& buf, bool is_header) {
    std::size_t k = kind_offset(is_header);
    std::size_t len = load_u16(buf.data() + k + 9);
    std::size_t start = k + CONTENT_OFFSET;
    if (start + len + 4 > PAGE_SIZE) return PageError::bad_checksum;
    std::uint32_t stored = load_u32(buf.data() + start + len);
    if (frame_crc(buf, k, len) != stored) return PageError::bad_checksum;
    DecodedPage d;
    if (is_header) {
        auto h = decode_header(buf.data());
        if (auto* e = std::get_if<PageError>(&h)) return *e;
        d.header = std::get<HeaderFields>(h);
    }
    d.kind = buf[k];
    d.epoch = load_u64(buf.data() + k + 1);
    d.content_start = start;
    d.content_end = start + len;
    return d;
}

}  // namespace kladde::store

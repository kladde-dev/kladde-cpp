// Unit tests of the format modules: CRC-32C, page framing, span-table
// entries, and the span id allocator.
#include <gtest/gtest.h>

#include <variant>

#include "../../src/store/crc.hpp"
#include "../../src/store/entries.hpp"
#include "../../src/store/page.hpp"
#include "../../src/store/state.hpp"
#include "../../src/store/util.hpp"
#include "test_util.hpp"

using namespace kladde::store;
using kladde::test::bytes;

// ---------------------------------------------------------------- crc

TEST(Crc, TheCheckValueFromTheSpecification) {
    EXPECT_EQ(crc32c(bytes("123456789")), 0xE3069283u);
    auto le = le_bytes(std::uint32_t{0xE3069283});
    EXPECT_EQ(le, (std::array<std::uint8_t, 4>{0x83, 0x92, 0x06, 0xE3}));
}

TEST(Crc, SlicingAgreesWithBytewiseAtEverySplit) {
    std::vector<std::uint8_t> data;
    for (std::uint32_t i = 0; i < 1000; ++i) data.push_back(static_cast<std::uint8_t>(i * 31 + 7));
    std::uint32_t whole = crc32c(data);
    for (std::size_t split : {0, 1, 7, 8, 9, 63, 500, 999, 1000}) {
        Crc32c c;
        c.update(std::span(data).first(split));
        c.update(std::span(data).subspan(split));
        EXPECT_EQ(c.value(), whole) << "split at " << split;
    }
    Crc32c c;
    for (std::uint8_t& b : data) c.update(std::span(&b, 1));
    EXPECT_EQ(c.value(), whole);
}

TEST(Crc, AMessageFollowedByItsOwnCrcEndsInAConstantState) {
    // The reason the journal's chain is blind to its own checksums.
    std::vector<std::uint32_t> residues;
    for (auto msg : {"a", "hello", "kladde journal"}) {
        Crc32c c;
        c.update(bytes(msg));
        c.update(le_bytes(c.value()));
        residues.push_back(c.value());
    }
    EXPECT_EQ(residues[0], residues[1]);
    EXPECT_EQ(residues[1], residues[2]);
}

// ---------------------------------------------------------------- page

TEST(Page, ADataPageRoundTrips) {
    auto buf = new_page();
    encode_page(*buf, nullptr, KIND_DATA, 7, bytes("hello"));
    auto d = std::get<DecodedPage>(decode_page(*buf, false));
    EXPECT_EQ(d.kind, KIND_DATA);
    EXPECT_EQ(d.epoch, 7u);
    EXPECT_EQ(std::vector<std::uint8_t>(buf->begin() + d.content_start, buf->begin() + d.content_end),
              bytes("hello"));
    EXPECT_EQ(d.content_start, CONTENT_OFFSET);
}

TEST(Page, AHeaderPageRoundTrips) {
    HeaderFields h;
    h.sequence_head = 3;
    h.schema_head = 4;
    h.private_head = 5;
    h.root_fingerprint.fill(9);
    h.journal_pointer = 12;
    auto buf = new_page();
    std::uint8_t content[] = {1, 2, 3};
    encode_page(*buf, &h, KIND_TABLE, 2, content);
    auto d = std::get<DecodedPage>(decode_page(*buf, true));
    ASSERT_TRUE(d.header);
    EXPECT_EQ(*d.header, h);
    EXPECT_EQ(d.epoch, 2u);
    EXPECT_EQ(d.content_start, HEADER_CONTENT_OFFSET);
}

TEST(Page, CorruptionAndAWildContentSizeAreRejected) {
    auto buf = new_page();
    std::vector<std::uint8_t> content(100, 5);
    encode_page(*buf, nullptr, KIND_DATA, 1, content);
    PageBuf bad = *buf;
    bad[CONTENT_OFFSET + 50] ^= 1;
    EXPECT_EQ(std::get<PageError>(decode_page(bad, false)), PageError::bad_checksum);
    PageBuf wild = *buf;
    store_u16(wild.data() + 9, 4090);
    EXPECT_EQ(std::get<PageError>(decode_page(wild, false)), PageError::bad_checksum);
    // The padding is not covered.
    PageBuf padded = *buf;
    padded[PAGE_SIZE - 1] = 0xAA;
    EXPECT_TRUE(std::holds_alternative<DecodedPage>(decode_page(padded, false)));
}

TEST(Page, AFullPageFitsExactly) {
    auto buf = new_page();
    std::vector<std::uint8_t> content(MAX_PAGE_CONTENT, 7);
    encode_page(*buf, nullptr, KIND_DATA, 1, content);
    EXPECT_TRUE(std::holds_alternative<DecodedPage>(decode_page(*buf, false)));
    std::vector<std::uint8_t> header_content(MAX_HEADER_CONTENT, 7);
    HeaderFields h;
    encode_page(*buf, &h, KIND_TABLE, 1, header_content);
    EXPECT_TRUE(std::holds_alternative<DecodedPage>(decode_page(*buf, true)));
}

TEST(Page, AFileWithoutTheMagicIsNoKladdeFile) {
    auto buf = new_page();
    HeaderFields h;
    encode_page(*buf, &h, KIND_TABLE, 0, {});
    (*buf)[3] = 'X';
    // The CRC covers the header, so a changed magic fails the checksum first.
    EXPECT_EQ(std::get<PageError>(decode_page(*buf, true)), PageError::bad_checksum);
}

// ---------------------------------------------------------------- entries

TEST(Entries, PagesRoundTripAndEstimatesAreClose) {
    std::vector<std::uint8_t> payload(40, 7);
    std::uint64_t a = 3 * PAGE_SIZE + CONTENT_OFFSET;
    std::vector<EntryOut> entries = {
        {1, 2, Body::ref(100, a)},
        {2, 3, Body::ref(1000, a + 100)},
        {3, 900, Body::inline_(payload)},
        {900, 0, Body::zero(1 << 20)},
        {901, 4, Body::ref(4081, 7 * PAGE_SIZE + CONTENT_OFFSET)},
    };
    std::vector<std::uint8_t> out;
    std::uint32_t children[] = {2, 5, 9};
    auto placed = encode_entries(children, entries, out);
    auto d = decode_entries(out);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->children, (std::vector<std::uint32_t>{2, 5, 9}));
    ASSERT_EQ(d->entries.size(), entries.size());
    for (std::size_t k = 0; k < entries.size(); ++k) {
        EntryOut const& e = entries[k];
        EntryIn const& i = d->entries[k];
        EXPECT_EQ(e.id, i.id);
        EXPECT_EQ(e.next, i.next);
        EXPECT_EQ(placed[k].len, i.entry_len);
        ASSERT_EQ(e.body.kind, i.kind);
        EXPECT_EQ(e.body.len, i.len);
        if (e.body.kind == Body::Kind::ref) {
            EXPECT_EQ(e.body.addr, i.addr);
        }
        if (e.body.kind == Body::Kind::inline_) {
            EXPECT_EQ(std::vector<std::uint8_t>(out.begin() + static_cast<std::ptrdiff_t>(i.offset),
                                                out.begin() + static_cast<std::ptrdiff_t>(i.offset + i.len)),
                      payload);
            EXPECT_EQ(i.offset, placed[k].payload);
        }
        std::size_t est = estimate(e.id, e.next, e.body);
        std::size_t len = placed[k].len;
        EXPECT_LE(est > len ? est - len : len - est, 3u) << "entry " << e.id;
    }
    // A reference of a page's length that follows its predecessor in id and
    // in the page takes five bytes.
    EXPECT_EQ(placed[1].len, 5);
}

TEST(Entries, MalformedPagesAreRefused) {
    // A reference into the header slots.
    std::vector<std::uint8_t> out;
    EntryOut e{1, 0, Body::ref(4, 100)};
    encode_entries({}, std::span(&e, 1), out);
    EXPECT_FALSE(decode_entries(out));
    // A Zero of one byte, then next = zigzag(0), which ends the chain.
    std::vector<std::uint8_t> ok = {0, 1, 2, 1, 0};
    EXPECT_TRUE(decode_entries(ok));
    // A truncated inline payload.
    std::vector<std::uint8_t> truncated = {0, 1, 20, 1, 2};
    EXPECT_FALSE(decode_entries(truncated));
}

TEST(Entries, TheWorkedExampleOfTheSpecificationDecodes) {
    // spec/span-table.md#a-worked-example
    std::vector<std::uint8_t> content = {0x00, 0x01, 0x00, 0x19, 0x9C, 0x80, 0x01, 0x00, 0x01,
                                         0x14, 'W',  'o',  'r',  'l',  'd',  ' ',  'm',  'a',
                                         'p',  0x01};
    auto d = decode_entries(content);
    ASSERT_TRUE(d);
    ASSERT_EQ(d->entries.size(), 2u);
    EXPECT_EQ(d->entries[0].id, 1u);
    EXPECT_EQ(d->entries[0].kind, Body::Kind::ref);
    EXPECT_EQ(d->entries[0].len, 25u);
    EXPECT_EQ(d->entries[0].addr, 8206u);
    EXPECT_EQ(d->entries[0].next, 0u);
    EXPECT_EQ(d->entries[1].id, 2u);
    EXPECT_EQ(d->entries[1].kind, Body::Kind::inline_);
    EXPECT_EQ(d->entries[1].len, 9u);
    EXPECT_EQ(d->entries[1].next, 1u);
}

// ---------------------------------------------------------------- ids

TEST(Ids, IdsGoLowestFirstAndFollowTheirPredecessor) {
    Ids ids;
    for (std::uint32_t k = 1; k <= 200; ++k) EXPECT_EQ(ids.alloc(0), k);
    for (std::uint32_t id : {5u, 70u, 130u}) ids.free(id);
    EXPECT_EQ(ids.alloc(0), 5u);
    EXPECT_EQ(ids.alloc(69), 70u);
    EXPECT_EQ(ids.alloc(0), 130u);
    EXPECT_EQ(ids.alloc(0), 201u);
    EXPECT_EQ(ids.count(), 201u);
}

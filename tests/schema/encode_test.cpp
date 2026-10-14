// The canonical encoding (spec/schema/canonical-encoding), and what its
// decoder refuses.
#include <kladde/schema.hpp>

#include <gtest/gtest.h>

#include <vector>

#include "helpers.hpp"

using namespace kladde::schema;
using namespace kladde::schema::test;
using Bytes = std::vector<std::uint8_t>;

namespace {

void expect_round_trip(TypeTable const& table) {
    auto decoded = TypeTable::decode(table.encode());
    ASSERT_TRUE(decoded) << decoded.error().what();
    EXPECT_EQ(*decoded, table);
}

// The decoder's message, or "" if it accepts `bytes`.
std::string refusal(Bytes const& bytes) {
    auto decoded = TypeTable::decode(bytes);
    if (decoded) return "";
    EXPECT_EQ(decoded.error().code, kladde::errc::corrupt);
    return decoded.error().message;
}

}  // namespace

TEST(Encode, RoundTripsEveryKind) {
    TypeTable table({
        Enum{"Value", 4,
             {variant(0, "Nil", 3), variant(1, "Num", 1), variant(2, "List", 2), variant(3, "Dict", 4)}},
        Primitive::i64,
        Opaque{"kladde-types", "DurableVec", {0, 1, 0}, std::nullopt, {ref(0)}},
        strukt("Unit", {}),
        Map{ref(5), ref(0)},
        Text{},
    });
    expect_round_trip(table);
}

TEST(Encode, AMapIsItsTagAndTwoReferences) {
    auto table = text_to_u32();
    EXPECT_EQ(table.encode(), (Bytes{3, 132, 1, 2, 134, 2}));
    expect_round_trip(table);
}

TEST(Encode, RoundTripsTheStructuralKinds) {
    // A struct holding a name, a list of counts, a fixed-size count, and an
    // array of three bytes.
    TypeTable table({
        strukt("Named", {field("name", 1), field("counts", 2), field("total", 4), field("rgb", 5)}),
        Text{},
        Sequence{ref(3)},
        Primitive::u32,
        Fixed{ref(3)},
        Array{ref(6), 3},
        Primitive::u8,
    });
    auto bytes = table.encode();
    // Text, Sequence, u32, Fixed, Array, u8: each a tag and its references,
    // and the array its length.
    Bytes tail(bytes.end() - 10, bytes.end());
    EXPECT_EQ(tail, (Bytes{134, 133, 3, 2, 135, 3, 131, 6, 3, 0}));
    expect_round_trip(table);
}

TEST(Encode, RoundTripsBothSizeModesOfOpaqueTypes) {
    for (auto size : {std::optional<std::uint64_t>{}, std::optional<std::uint64_t>{0},
                      std::optional<std::uint64_t>{16}}) {
        TypeTable table({Opaque{"lib", "T", {1, 0, 0}, size, {}}});
        expect_round_trip(table);
    }
    // The size mode is one more than the size, and 0 without one.
    EXPECT_EQ(TypeTable({Opaque{"l", "T", {1, 2, 3}, 16, {}}}).encode(),
              (Bytes{1, 130, 1, 'l', 1, 'T', 1, 2, 3, 17, 0}));
    EXPECT_EQ(TypeTable({Opaque{"l", "T", {1, 2, 3}, std::nullopt, {}}}).encode(),
              (Bytes{1, 130, 1, 'l', 1, 'T', 1, 2, 3, 0, 0}));
}

TEST(Encode, PointHasTheExpectedBytes) {
    Bytes expected{
        2,                                   // descriptor count
        128, 5, 'P', 'o', 'i', 'n', 't', 2,  // Struct "Point", 2 fields
        1, 'x', 1,                           // field "x" -> ref 1
        1, 'y', 1,                           // field "y" -> ref 1
        6,                                   // Primitive i32
    };
    EXPECT_EQ(point().encode(), expected);
}

TEST(Encode, SpecWorkedExamples) {
    // canonical-encoding.md#table-encoding: a vector of text.
    EXPECT_EQ(TypeTable({Sequence{ref(1)}, Text{}}).encode(), (Bytes{0x02, 0x85, 0x01, 0x86}));
    // canonical-encoding.md#worked-example: `struct Point { x: i32, y: u8 }`.
    TypeTable p({strukt("Point", {field("x", 1), field("y", 2)}), Primitive::i32, Primitive::u8});
    EXPECT_EQ(p.encode(), (Bytes{0x03, 0x80, 0x05, 0x50, 0x6f, 0x69, 0x6e, 0x74, 0x02, 0x01, 0x78, 0x01, 0x01,
                                 0x79, 0x02, 0x06, 0x00}));
}

TEST(Encode, EnumEncodesNameWidthAndVariantsInAscendingOrder) {
    TypeTable table({Enum{"E", 2, {variant(300, "B", 1), variant(5, "A", 1)}}, strukt("", {})});
    EXPECT_EQ(table.encode(), (Bytes{2, 129, 1, 'E', 2, 2, 5, 1, 'A', 1, 0xac, 0x02, 1, 'B', 1, 128, 0, 0}));
}

TEST(Encode, VariantOrderDoesNotAffectEncoding) {
    TypeTable ascending({Enum{"E", 4, {variant(0, "A", 1), variant(1, "B", 1)}}, strukt("Unit", {})});
    TypeTable descending({Enum{"E", 4, {variant(1, "B", 1), variant(0, "A", 1)}}, strukt("Unit", {})});
    EXPECT_EQ(ascending.encode(), descending.encode());
    // Decoding gives the canonical order back.
    EXPECT_EQ(TypeTable::decode(descending.encode()).value(), ascending);
}

TEST(Encode, RejectsUnknownKinds) {
    EXPECT_NE(refusal({1, 136}).find("unknown kind tag 136"), std::string::npos);
    EXPECT_NE(refusal({1, 255}).find("unknown kind tag 255"), std::string::npos);
}

TEST(Encode, RejectsReservedPrimitiveCodes) {
    EXPECT_EQ(refusal({1, 11}), "");
    EXPECT_NE(refusal({1, 12}).find("unsupported primitive code 12"), std::string::npos);
    EXPECT_NE(refusal({1, 127}).find("unsupported primitive code 127"), std::string::npos);
}

TEST(Encode, RejectsOutOfRangeReference) {
    TypeTable table({strukt("S", {field("f", 5)})});
    EXPECT_NE(refusal(table.encode()).find("out of range"), std::string::npos);
    EXPECT_FALSE(table.validate());
}

TEST(Encode, RejectsTrailingBytes) {
    auto bytes = point().encode();
    bytes.push_back(0xff);
    EXPECT_NE(refusal(bytes).find("bytes follow the last descriptor"), std::string::npos);
}

TEST(Encode, RejectsAnEmptyTable) {
    EXPECT_NE(refusal({0}).find("no descriptors"), std::string::npos);
    EXPECT_NE(refusal({}).find("truncated"), std::string::npos);
    EXPECT_FALSE(TypeTable().validate());
}

TEST(Encode, RejectsEveryTruncation) {
    TypeTable table({
        Enum{"Value", 4, {variant(0, "Nil", 3), variant(1, "Num", 1), variant(2, "Op", 2)}},
        Primitive::i64,
        Opaque{"lib", "T", {0, 1, 0}, 4, {ref(1)}},
        strukt("Unit", {}),
    });
    auto bytes = table.encode();
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        EXPECT_NE(refusal(Bytes(bytes.begin(), bytes.begin() + std::ptrdiff_t(n))), "") << n << " bytes";
    }
}

TEST(Encode, RejectsNonMinimalVarints) {
    // A vector of text, with the count, then the reference, written in two
    // bytes.
    EXPECT_NE(refusal({0x82, 0x00, 0x85, 0x01, 0x86}).find("longer than its value needs"), std::string::npos);
    EXPECT_NE(refusal({0x02, 0x85, 0x81, 0x00, 0x86}).find("longer than its value needs"), std::string::npos);
}

TEST(Encode, RejectsInvalidUtf8) {
    // Struct with name "\xc3" (a lead byte without its continuation).
    EXPECT_NE(refusal({1, 128, 1, 0xc3, 0}).find("UTF-8"), std::string::npos);
    // Surrogates and overlong forms are not UTF-8 either.
    EXPECT_NE(refusal({1, 128, 3, 0xed, 0xa0, 0x80, 0}).find("UTF-8"), std::string::npos);
    EXPECT_NE(refusal({1, 128, 2, 0xc0, 0x80, 0}).find("UTF-8"), std::string::npos);
    EXPECT_EQ(refusal({1, 128, 2, 0xc3, 0xa4, 0}), "");
}

TEST(Encode, RejectsInvalidDiscriminantWidths) {
    for (std::uint8_t width = 0; width < 10; ++width) {
        Bytes bytes{1, 129, 0, width, 0};
        bool valid = width == 1 || width == 2 || width == 4 || width == 8;
        EXPECT_EQ(refusal(bytes).empty(), valid) << int(width);
    }
}

TEST(Encode, RejectsVariantsOutOfCanonicalOrder) {
    // Two variants, discriminants 1 then 0, both holding the empty Struct.
    Bytes descending{2, 129, 0, 1, 2, 1, 1, 'B', 1, 0, 1, 'A', 1, 128, 0, 0};
    EXPECT_NE(refusal(descending).find("ascending"), std::string::npos);
    Bytes duplicate{2, 129, 0, 1, 2, 0, 1, 'B', 1, 0, 1, 'A', 1, 128, 0, 0};
    EXPECT_NE(refusal(duplicate).find("ascending"), std::string::npos);
    Bytes ascending{2, 129, 0, 1, 2, 0, 1, 'A', 1, 1, 1, 'B', 1, 128, 0, 0};
    EXPECT_EQ(refusal(ascending), "");
}

TEST(Encode, RejectsADiscriminantWiderThanItsWidth) {
    Bytes bytes{2, 129, 0, 1, 1, 0x80, 0x02, 0, 1, 128, 0, 0};  // discriminant 256 in one byte
    EXPECT_NE(refusal(bytes).find("discriminant width"), std::string::npos);
}

TEST(Encode, RejectsAnOpaqueSizeModeBeyond64Bits) {
    // The size mode of a fixed size 2^64 - 1 would be 2^64; in memory, a
    // table that holds one is refused.
    TypeTable table({Opaque{"l", "T", {1, 0, 0}, std::numeric_limits<std::uint64_t>::max(), {}}});
    EXPECT_FALSE(table.validate());
    // The largest size mode holds 2^64 - 2 bytes.
    Bytes bytes{1, 130, 1, 'l', 1, 'T', 1, 0, 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0};
    auto decoded = TypeTable::decode(bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(std::get<Opaque>(decoded->get(ref(0))).fixed_size, std::numeric_limits<std::uint64_t>::max() - 1);
}

TEST(Encode, DecodesALongChainWithoutRecursing) {
    // 30 000 nested sequences: decoding validates them, and none of the
    // walks over the graph may recurse once per level.
    constexpr std::size_t n = 30000;
    std::vector<TypeDescriptor> ds;
    for (std::size_t i = 0; i + 1 < n; ++i) ds.push_back(Sequence{ref(i + 1)});
    ds.push_back(Primitive::u8);
    TypeTable table(std::move(ds));
    auto decoded = TypeTable::decode(table.encode());
    ASSERT_TRUE(decoded) << decoded.error().what();
    EXPECT_EQ(decoded->fingerprint(), table.fingerprint());
    EXPECT_EQ(decoded->fixed_size(ref(0)), std::nullopt);

    // The same as a chain of structs, which has a fixed size.
    std::vector<TypeDescriptor> structs;
    for (std::size_t i = 0; i + 1 < n; ++i) structs.push_back(strukt("S", {field("f", i + 1)}));
    structs.push_back(Primitive::u8);
    TypeTable chain(std::move(structs));
    EXPECT_TRUE(chain.validate());
    EXPECT_EQ(chain.fixed_size(ref(0)), 1u);
}

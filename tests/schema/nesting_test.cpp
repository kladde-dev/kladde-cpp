// Fixed sizes, fixed forms, the rules of nesting
// (spec/schema/type-descriptors#rules-of-nesting), and measuring encodings.
#include <kladde/schema.hpp>

#include <gtest/gtest.h>

#include <vector>

#include "helpers.hpp"

using namespace kladde::schema;
using namespace kladde::schema::test;
using Bytes = std::vector<std::uint8_t>;

namespace {

std::string nesting_error(TypeTable const& table) {
    auto r = table.validate();
    if (r) return "";
    EXPECT_EQ(r.error().code, kladde::errc::corrupt);
    return r.error().message;
}

/// `enum Shape { Circle(f32), Rect(f32, f32) }` at [0], `f32` at [1], and
/// `Rect`'s payload at [2].
std::vector<TypeDescriptor> shape() {
    return {
        Enum{"Shape", 2, {variant(0, "V0", 1), variant(1, "V1", 2)}},
        Primitive::f32,
        strukt("Rect", {field("0", 1), field("1", 1)}),
    };
}

std::optional<std::size_t> measure(TypeTable const& t, std::size_t r, Bytes const& b) { return t.measure(ref(r), b); }

}  // namespace

TEST(Nesting, PrimitiveSizes) {
    EXPECT_EQ(fixed_size(Primitive::f32), 4u);
    EXPECT_EQ(fixed_size(Primitive::u32), std::nullopt);
    EXPECT_EQ(fixed_size(Primitive::character), std::nullopt);
    EXPECT_EQ(fixed_form_size(Primitive::u16), 2u);
    EXPECT_EQ(fixed_form_size(Primitive::character), 4u);
}

TEST(Nesting, FixedSizesFollowTheEncoding) {
    TypeTable table(shape());
    EXPECT_EQ(table.fixed_size(ref(0)), std::nullopt);
    EXPECT_EQ(table.fixed_form_size(ref(0)), 2u + 8u);
    // With variants of one size, the enum is fixed-size, its discriminant
    // one byte.
    auto d = shape();
    d[0] = Enum{"Pair", 4, {variant(0, "V0", 1), variant(127, "V127", 1)}};
    EXPECT_EQ(TypeTable(d).fixed_size(ref(0)), 5u);
    // A discriminant of 128 takes two bytes, so the size depends on it.
    std::get<Enum>(d[0]).variants[1].discriminant = 128;
    EXPECT_EQ(TypeTable(d).fixed_size(ref(0)), std::nullopt);
}

TEST(Nesting, FixedSizesOfEveryKind) {
    TypeTable point_f32({strukt("Point", {field("x", 1), field("y", 1)}), Primitive::f32});
    EXPECT_EQ(point_f32.fixed_size(ref(0)), 8u);
    TypeTable table({
        Array{ref(1), 3},                              // 0
        Primitive::u8,                                 // 1
        Sequence{ref(1)},                              // 2
        Map{ref(1), ref(1)},                           // 3
        Text{},                                        // 4
        Opaque{"l", "T", {1, 0, 0}, 12, {ref(2)}},     // 5
        Opaque{"l", "T", {1, 0, 0}, std::nullopt, {}}, // 6
        Fixed{ref(8)},                                 // 7
        Primitive::u64,                                // 8
        strukt("Unit", {}),                            // 9
        Enum{"Never", 1, {}},                          // 10
    });
    EXPECT_EQ(table.fixed_size(ref(0)), 3u);
    EXPECT_EQ(table.fixed_size(ref(2)), std::nullopt);
    EXPECT_EQ(table.fixed_size(ref(3)), std::nullopt);
    EXPECT_EQ(table.fixed_size(ref(4)), std::nullopt);
    EXPECT_EQ(table.fixed_size(ref(5)), 12u);
    EXPECT_EQ(table.fixed_form_size(ref(5)), 12u);
    EXPECT_EQ(table.fixed_size(ref(6)), std::nullopt);
    EXPECT_EQ(table.fixed_form_size(ref(6)), std::nullopt);
    EXPECT_EQ(table.fixed_size(ref(7)), 8u);
    EXPECT_EQ(table.fixed_size(ref(8)), std::nullopt);
    EXPECT_EQ(table.fixed_form_size(ref(8)), 8u);
    EXPECT_EQ(table.fixed_size(ref(9)), 0u);
    EXPECT_EQ(table.fixed_size(ref(10)), std::nullopt);
    EXPECT_EQ(table.fixed_form_size(ref(10)), std::nullopt);
    EXPECT_TRUE(table.validate());
}

TEST(Nesting, OptionHasAFixedFormButNoFixedSize) {
    TypeTable option({
        Enum{"Option", 1, {variant(0, "None", 2), variant(1, "Some", 1)}},
        Primitive::u32,
        strukt("Unit", {}),
    });
    EXPECT_EQ(option.fixed_size(ref(0)), std::nullopt);
    EXPECT_EQ(option.fixed_form_size(ref(0)), 5u);
}

TEST(Nesting, ATypeMetInsideAndOutsideAFixedKeepsBothSizes) {
    // `struct S { b: Fixed(u32), a: u32 }`: the u32 has a fixed form of four
    // bytes, which does not make it fixed-size outside the Fixed.
    TypeTable table({strukt("S", {field("b", 1), field("a", 2)}), Fixed{ref(2)}, Primitive::u32});
    EXPECT_EQ(table.fixed_size(ref(0)), std::nullopt);
    EXPECT_EQ(table.fixed_form_size(ref(0)), 8u);
    TypeTable reversed({strukt("S", {field("a", 2), field("b", 1)}), Fixed{ref(2)}, Primitive::u32});
    EXPECT_EQ(reversed.fixed_size(ref(0)), std::nullopt);
}

TEST(Nesting, SizesThatOverflowAreNone) {
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    TypeTable table({Array{ref(1), max}, Primitive::u16, Array{ref(3), 2}, Primitive::u8,
                     strukt("S", {field("a", 5), field("b", 5)}), Opaque{"l", "T", {1, 0, 0}, max - 1, {}}});
    EXPECT_EQ(table.fixed_size(ref(0)), std::nullopt);
    EXPECT_EQ(table.fixed_form_size(ref(0)), std::nullopt);
    EXPECT_EQ(table.fixed_size(ref(2)), 2u);
    EXPECT_EQ(table.fixed_size(ref(4)), std::nullopt);
    // An array of zero-sized elements is zero-sized however long.
    TypeTable empty({Array{ref(1), max}, strukt("Unit", {})});
    EXPECT_EQ(empty.fixed_size(ref(0)), 0u);
    EXPECT_EQ(empty.measure(ref(0), Bytes{}), 0u);
}

TEST(Nesting, AFixedWrapperNeedsAFixedForm) {
    TypeTable seq({Fixed{ref(1)}, Sequence{ref(2)}, Primitive::u8});
    EXPECT_NE(nesting_error(seq).find("a Fixed wrapper holds a type without a fixed form"), std::string::npos);
    TypeTable u64({Fixed{ref(1)}, Primitive::u64});
    EXPECT_TRUE(u64.validate());
    EXPECT_EQ(u64.fixed_size(ref(0)), 8u);
    // A tree has no fixed form, so `Fixed(Tree)` does not exist.
    TypeTable fixed_tree({Fixed{ref(1)}, strukt("Tree", {field("children", 2)}), Sequence{ref(1)}});
    EXPECT_NE(nesting_error(fixed_tree), "");
    TypeTable never({Fixed{ref(1)}, Enum{"Never", 1, {}}});
    EXPECT_NE(nesting_error(never).find("fixed form"), std::string::npos);
}

TEST(Nesting, RecursionNeedsAnEnumASequenceOrAMap) {
    TypeTable self({strukt("S", {field("s", 0)})});
    EXPECT_NE(nesting_error(self).find("contains itself"), std::string::npos);
    // `enum List { Nil, Cons(u8, List) }` holds itself inline.
    TypeTable list_u8({
        Enum{"List", 1, {variant(0, "V0", 2), variant(1, "V1", 3)}},
        Primitive::u8,
        strukt("Unit", {}),
        strukt("Cons", {field("0", 1), field("1", 0)}),
    });
    EXPECT_TRUE(list_u8.validate());
    EXPECT_EQ(list_u8.fixed_form_size(ref(0)), std::nullopt);
    EXPECT_EQ(measure(list_u8, 0, {1, 7, 1, 8, 0}), 5u);
    // A tree whose nodes hold their children inline.
    TypeTable tree({strukt("Tree", {field("label", 1), field("children", 2)}), Text{}, Sequence{ref(0)}});
    EXPECT_TRUE(tree.validate());
    TypeTable map_tree({strukt("Tree", {field("children", 1)}), Map{ref(2), ref(0)}, Text{}});
    EXPECT_TRUE(map_tree.validate());
    // Arrays and Fixed wrappers are inline too.
    TypeTable array({Array{ref(1), 2}, strukt("S", {field("a", 0)})});
    EXPECT_NE(nesting_error(array).find("contains itself"), std::string::npos);
    TypeTable fixed({Fixed{ref(1)}, strukt("S", {field("f", 0)})});
    EXPECT_NE(nesting_error(fixed), "");
    TypeTable mutual({strukt("A", {field("b", 1)}), strukt("B", {field("a", 0)})});
    EXPECT_NE(nesting_error(mutual).find("contains itself"), std::string::npos);
}

TEST(Nesting, ADiscriminantMustFitItsWidth) {
    TypeTable table({Enum{"E", 1, {variant(256, "V", 1)}}, strukt("Unit", {})});
    EXPECT_NE(nesting_error(table).find("discriminant width"), std::string::npos);
    TypeTable fits({Enum{"E", 1, {variant(255, "V", 1)}}, strukt("Unit", {})});
    EXPECT_TRUE(fits.validate());
    TypeTable wide({Enum{"E", 8, {variant(std::numeric_limits<std::uint64_t>::max(), "V", 1)}}, strukt("Unit", {})});
    EXPECT_TRUE(wide.validate());
}

TEST(Nesting, ValidateRefusesWhatNoEncodingHolds) {
    TypeTable width({Enum{"E", 3, {variant(0, "V", 1)}}, strukt("Unit", {})});
    EXPECT_NE(nesting_error(width).find("discriminant width"), std::string::npos);
    TypeTable twice({Enum{"E", 1, {variant(0, "A", 1), variant(0, "B", 1)}}, strukt("Unit", {})});
    EXPECT_NE(nesting_error(twice).find("share a discriminant"), std::string::npos);
    TypeTable code({static_cast<Primitive>(12)});
    EXPECT_NE(nesting_error(code).find("unsupported primitive code 12"), std::string::npos);
    TypeTable dangling({Map{ref(0), ref(1)}});
    EXPECT_NE(nesting_error(dangling).find("reference 1 is out of range"), std::string::npos);
    TypeTable opaque_param({Opaque{"l", "T", {1, 0, 0}, std::nullopt, {ref(4)}}});
    EXPECT_NE(nesting_error(opaque_param).find("out of range"), std::string::npos);
}

TEST(Nesting, DecodeAppliesTheRules) {
    TypeTable table({strukt("S", {field("s", 0)})});
    auto decoded = TypeTable::decode(table.encode());
    ASSERT_FALSE(decoded);
    EXPECT_NE(decoded.error().message.find("contains itself"), std::string::npos);
}

TEST(Measure, ReadsFixedFormsAndRefusesBadBytes) {
    auto d = shape();
    d.push_back(Fixed{ref(0)});
    TypeTable table(d);
    // `Fixed(Shape)`: a two-byte discriminant, then eight bytes.
    Bytes circle{0, 0, 0, 0, 0x80, 0x3f, 0, 0, 0, 0};
    EXPECT_EQ(measure(table, 3, circle), 10u);
    auto bad = circle;
    bad[9] = 1;  // padding must be zeros
    EXPECT_EQ(measure(table, 3, bad), std::nullopt);
    // A map: two entries of a text and a u8.
    TypeTable map({Map{ref(1), ref(2)}, Text{}, Primitive::u8});
    EXPECT_EQ(measure(map, 0, {2, 'a', 0xff, 1, 'b', 0xff, 2}), 7u);
    EXPECT_EQ(map.fixed_form_size(ref(0)), std::nullopt);
    // Text needs its terminator and valid UTF-8; a varint must be minimal.
    TypeTable text({Text{}});
    EXPECT_EQ(measure(text, 0, {'a', 'b', 0xff}), 3u);
    EXPECT_EQ(measure(text, 0, {'a', 'b'}), std::nullopt);
    EXPECT_EQ(measure(text, 0, {0xc3, 0xff}), std::nullopt);
    TypeTable u16({Primitive::u16});
    EXPECT_EQ(measure(u16, 0, {0x80, 0x00}), std::nullopt);
    EXPECT_EQ(measure(u16, 0, {0xff, 0xff, 0x04}), std::nullopt);
    EXPECT_EQ(measure(u16, 0, {0xff, 0xff, 0x03}), 3u);
}

TEST(Measure, Primitives) {
    TypeTable t({Primitive::boolean, Primitive::character, Primitive::f64, Primitive::i32, Fixed{ref(1)},
                 Fixed{ref(0)}, Fixed{ref(3)}});
    EXPECT_EQ(measure(t, 0, {1}), 1u);
    EXPECT_EQ(measure(t, 0, {2}), std::nullopt);
    EXPECT_EQ(measure(t, 1, {'a'}), 1u);
    EXPECT_EQ(measure(t, 1, {0xe2, 0x82, 0xac, 'x'}), 3u);  // the euro sign
    EXPECT_EQ(measure(t, 1, {0xe2, 0x82}), std::nullopt);
    EXPECT_EQ(measure(t, 1, {0xed, 0xa0, 0x80}), std::nullopt);  // a surrogate
    EXPECT_EQ(measure(t, 1, {0x80}), std::nullopt);
    EXPECT_EQ(measure(t, 2, Bytes(8)), 8u);
    EXPECT_EQ(measure(t, 2, Bytes(7)), std::nullopt);
    EXPECT_EQ(measure(t, 3, {0xff, 0xff, 0xff, 0xff, 0x0f}), 5u);
    EXPECT_EQ(measure(t, 3, {0x80, 0x80, 0x80, 0x80, 0x10}), std::nullopt);
    // A char's fixed form is a Unicode scalar value in four bytes.
    EXPECT_EQ(measure(t, 4, {0xac, 0x20, 0, 0}), 4u);
    EXPECT_EQ(measure(t, 4, {0x00, 0xd8, 0, 0}), std::nullopt);
    EXPECT_EQ(measure(t, 4, {0x00, 0x00, 0x11, 0}), std::nullopt);
    EXPECT_EQ(measure(t, 5, {1}), 1u);
    EXPECT_EQ(measure(t, 5, {2}), std::nullopt);
    EXPECT_EQ(measure(t, 6, {0xff, 0xff, 0xff, 0xff}), 4u);
}

TEST(Measure, SpecWorkedExamples) {
    // type-descriptors.md#a-worked-example: `struct Point { x: f32, label:
    // Text, n: u32 }` holding (1.5, "ab", 300).
    TypeTable point_t({strukt("Point", {field("x", 1), field("label", 2), field("n", 3)}), Primitive::f32, Text{},
                       Primitive::u32});
    EXPECT_EQ(measure(point_t, 0, {0x00, 0x00, 0xc0, 0x3f, 0x61, 0x62, 0xff, 0xac, 0x02}), 9u);
    // `Fixed(Option<u32>)` holding Some(300), and None.
    TypeTable option({
        Fixed{ref(1)},
        Enum{"Option", 1, {variant(0, "None", 3), variant(1, "Some", 2)}},
        Primitive::u32,
        strukt("Unit", {}),
    });
    EXPECT_EQ(measure(option, 0, {0x01, 0x2c, 0x01, 0x00, 0x00}), 5u);
    EXPECT_EQ(measure(option, 0, {0x00, 0x00, 0x00, 0x00, 0x00}), 5u);
    EXPECT_EQ(measure(option, 0, {0x00, 0x00, 0x00, 0x01, 0x00}), std::nullopt);
    EXPECT_EQ(measure(option, 0, {0x02, 0x00, 0x00, 0x00, 0x00}), std::nullopt);
    // `Option<Fixed<u32>>`: one byte for None, five for Some.
    TypeTable option_fixed({
        Enum{"Option", 1, {variant(0, "None", 3), variant(1, "Some", 1)}},
        Fixed{ref(2)},
        Primitive::u32,
        strukt("Unit", {}),
    });
    EXPECT_EQ(measure(option_fixed, 0, {0x00, 0xaa}), 1u);
    EXPECT_EQ(measure(option_fixed, 0, {0x01, 0x2c, 0x01, 0x00, 0x00}), 5u);
}

TEST(Measure, OpaqueArraysAndSequences) {
    TypeTable t({
        Opaque{"l", "T", {1, 0, 0}, 3, {}},
        Opaque{"l", "T", {1, 0, 0}, std::nullopt, {}},
        Array{ref(3), 2},
        Primitive::u32,
        Sequence{ref(5)},
        strukt("Unit", {}),
    });
    EXPECT_EQ(measure(t, 0, {9, 9, 9, 9}), 3u);
    EXPECT_EQ(measure(t, 0, {9, 9}), std::nullopt);
    EXPECT_EQ(measure(t, 1, {2, 9, 9, 9}), 3u);
    EXPECT_EQ(measure(t, 1, {3, 9, 9}), std::nullopt);
    EXPECT_EQ(measure(t, 2, {1, 0xac, 0x02}), 3u);
    // A sequence of empty structs takes its count alone, whatever the count.
    EXPECT_EQ(measure(t, 4, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}), 10u);
}

TEST(Measure, RefusesNestingBeyondItsLimit) {
    // `enum List { Nil, Cons(List) }`, nested deeper than the limit.
    TypeTable chain({Enum{"List", 1, {variant(0, "Nil", 1), variant(1, "Cons", 0)}}, strukt("Unit", {})});
    Bytes shallow(100, 1);
    shallow.push_back(0);
    EXPECT_EQ(measure(chain, 0, shallow), 101u);
    Bytes deep(TypeTable::max_measure_depth + 10, 1);
    deep.push_back(0);
    EXPECT_EQ(measure(chain, 0, deep), std::nullopt);
}

#include <kladde/schema.hpp>

#include <gtest/gtest.h>

#include "helpers.hpp"

using namespace kladde::schema;
using namespace kladde::schema::test;

TEST(Dump, OneLinePerDescriptor) {
    TypeTable table({
        Enum{"Value", 4, {variant(3, "Dict", 4), variant(0, "Nil", 3), variant(1, "Num", 1), variant(2, "List", 2)}},
        Primitive::i64,
        Opaque{"kladde-types", "DurableVec", {0, 1, 0}, std::nullopt, {ref(0), ref(1)}},
        strukt("Unit", {}),
        Map{ref(5), ref(0)},
        Text{},
        Array{ref(7), 3},
        Primitive::u8,
        Fixed{ref(7)},
        Sequence{ref(5)},
        strukt("Point", {field("x", 1), field("y", 1)}),
        Opaque{"lib", "T", {1, 2, 3}, 8, {}},
        Primitive::character,
        Enum{"Never", 1, {}},
    });
    EXPECT_EQ(dump(table),
              "[0] enum \"Value\" width 4 { 0 \"Nil\": #3, 1 \"Num\": #1, 2 \"List\": #2, 3 \"Dict\": #4 }\n"
              "[1] i64\n"
              "[2] opaque \"kladde-types\" \"DurableVec\" 0.1.0 size - <#0, #1>\n"
              "[3] struct \"Unit\" {}\n"
              "[4] map #5 -> #0\n"
              "[5] text\n"
              "[6] array #7 len 3\n"
              "[7] u8\n"
              "[8] fixed #7\n"
              "[9] sequence #5\n"
              "[10] struct \"Point\" { \"x\": #1, \"y\": #1 }\n"
              "[11] opaque \"lib\" \"T\" 1.2.3 size 8 <>\n"
              "[12] char\n"
              "[13] enum \"Never\" width 1 {}\n");
}

TEST(Dump, EveryPrimitive) {
    std::vector<TypeDescriptor> ds;
    for (std::uint8_t code = 0; code <= 12; ++code) ds.push_back(static_cast<Primitive>(code));
    EXPECT_EQ(dump(TypeTable(ds)),
              "[0] u8\n[1] u16\n[2] u32\n[3] u64\n[4] i8\n[5] i16\n[6] i32\n[7] i64\n[8] f32\n[9] f64\n"
              "[10] bool\n[11] char\n[12] primitive 12\n");
}

TEST(Dump, QuotesStrings) {
    TypeTable table({strukt("a\"b\\c", {field("\n\x7f", 1), field("\xc3\xa4", 1)}), Primitive::u8});
    EXPECT_EQ(dump(table), "[0] struct \"a\\\"b\\\\c\" { \"\\x0a\\x7f\": #1, \"\xc3\xa4\": #1 }\n[1] u8\n");
}

TEST(Dump, AgreesExactlyWhenEncodingsAgree) {
    TypeTable a({Enum{"E", 4, {variant(0, "A", 1), variant(1, "B", 1)}}, strukt("Unit", {})});
    TypeTable b({Enum{"E", 4, {variant(1, "B", 1), variant(0, "A", 1)}}, strukt("Unit", {})});
    EXPECT_EQ(dump(a), dump(b));
    TypeTable c({Enum{"E", 2, {variant(0, "A", 1), variant(1, "B", 1)}}, strukt("Unit", {})});
    EXPECT_NE(dump(a), dump(c));
    EXPECT_EQ(dump(TypeTable({Sequence{ref(1)}, Text{}})), "[0] sequence #1\n[1] text\n");
}

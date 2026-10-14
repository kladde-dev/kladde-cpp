// Fingerprints (spec/schema/fingerprints), and their vectors.
#include <kladde/schema.hpp>

#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "helpers.hpp"

using namespace kladde::schema;
using namespace kladde::schema::test;

namespace {

/// `struct A { b: B }`, `struct B { a: A }`: a two-node cycle.
TypeTable mutual_pair() { return TypeTable({strukt("A", {field("b", 1)}), strukt("B", {field("a", 0)})}); }

/// `enum Rec { Stop, Go(Rec) }`: a self-recursive enum.
TypeTable rec_enum() {
    return TypeTable({Enum{"Rec", 4, {variant(0, "Stop", 1), variant(1, "Go", 0)}}, strukt("Unit", {})});
}

/// An outer cycle, a self-referential struct or a mutual `A <-> B` pair, one
/// of whose members also holds an inner subtree (`Inner { mid: Mid, count:
/// i32 }`, `Mid { a: i32, b: i32 }`). Field order is significant, so each
/// combination of `mutual` and `inner_first` is a distinct type graph.
TypeTable outer_cycle_with_inner(bool mutual, bool inner_first) {
    if (mutual) {
        std::vector<Field> a_fields{field("b", 1), field("inner", 2)};
        if (inner_first) std::swap(a_fields[0], a_fields[1]);
        return TypeTable({
            strukt("A", a_fields),
            strukt("B", {field("a", 0)}),
            strukt("Inner", {field("mid", 3), field("count", 4)}),
            strukt("Mid", {field("a", 4), field("b", 4)}),
            Primitive::i32,
        });
    }
    std::vector<Field> outer_fields{field("next", 0), field("inner", 1)};
    if (inner_first) std::swap(outer_fields[0], outer_fields[1]);
    return TypeTable({
        strukt("Outer", outer_fields),
        strukt("Inner", {field("mid", 2), field("count", 3)}),
        strukt("Mid", {field("a", 3), field("b", 3)}),
        Primitive::i32,
    });
}

}  // namespace

TEST(Fingerprint, SpecVectors) {
    // spec/schema/fingerprints.md#vectors
    EXPECT_EQ(point().fingerprint().to_hex(), "0d91791f5827efadfa8150af9812fad6");
    EXPECT_EQ(list().fingerprint().to_hex(), "abdbb8e6f2b104819c9b608dd77aaedf");
    EXPECT_EQ(opaque_versioned(1, 2, 3).fingerprint().to_hex(), "81a2a3d8b939f4f216a344cf10758953");
    EXPECT_EQ(text_to_u32().fingerprint().to_hex(), "6a33dc4a1e560e465f7a50543f62c492");
}

TEST(Fingerprint, PointsHashInputIsTheSpecs) {
    // fingerprints.md: Point's hash input is `80 02 01 78 00 <i32> 01 79 00
    // <i32>`, and i32's the single byte `06`.
    std::uint8_t i32_input[] = {0x06};
    auto i32_digest = sha256(i32_input);
    std::vector<std::uint8_t> input{0x80, 0x02, 0x01, 0x78, 0x00};
    input.insert(input.end(), i32_digest.begin(), i32_digest.begin() + 16);
    input.insert(input.end(), {0x01, 0x79, 0x00});
    input.insert(input.end(), i32_digest.begin(), i32_digest.begin() + 16);
    auto digest = sha256(input);
    Fingerprint expected;
    std::copy_n(digest.begin(), 16, expected.bytes.begin());
    EXPECT_EQ(point().fingerprint(), expected);
    EXPECT_EQ(point().fingerprint(ref(1)), TypeTable({Primitive::i32}).fingerprint());
}

TEST(Fingerprint, ABackEdgeIsItsDistanceUp) {
    // `enum Rec { Stop, Go(Rec) }`: Rec's input is its tag, width and count,
    // then per variant its discriminant, name, and token; Go's token is the
    // back edge `01 00` to Rec itself, zero levels up.
    std::uint8_t unit_input[] = {0x80, 0x00};
    auto unit = sha256(unit_input);
    std::vector<std::uint8_t> input{0x81, 0x04, 0x02, 0x00, 0x04, 'S', 't', 'o', 'p', 0x00};
    input.insert(input.end(), unit.begin(), unit.begin() + 16);
    input.insert(input.end(), {0x01, 0x02, 'G', 'o', 0x01, 0x00});
    auto digest = sha256(input);
    Fingerprint expected;
    std::copy_n(digest.begin(), 16, expected.bytes.begin());
    EXPECT_EQ(rec_enum().fingerprint(), expected);
}

TEST(Fingerprint, Reproducible) {
    EXPECT_EQ(point().fingerprint(), point().fingerprint());
    EXPECT_EQ(list().fingerprint(), list().fingerprint());
}

TEST(Fingerprint, IndexInvariant) {
    // `struct Pair(i32, u64)` with its leaves at 1 and 2, then swapped.
    TypeTable a({strukt("Pair", {field("0", 1), field("1", 2)}), Primitive::i32, Primitive::u64});
    TypeTable b({strukt("Pair", {field("0", 2), field("1", 1)}), Primitive::u64, Primitive::i32});
    EXPECT_EQ(a.fingerprint(), b.fingerprint());
    // A cycle, renumbered: List's descriptors in another order.
    TypeTable renumbered({
        Enum{"List", 4, {variant(1, "Cons", 1), variant(0, "Nil", 3)}},
        strukt("Cons", {field("0", 2), field("1", 0)}),
        Primitive::i32,
        strukt("Unit", {}),
    });
    EXPECT_EQ(renumbered.fingerprint(), list().fingerprint());
}

TEST(Fingerprint, StructurallySensitive) {
    auto base = point().fingerprint();
    // Reordered fields: different.
    EXPECT_NE(base, TypeTable({strukt("Point", {field("y", 1), field("x", 1)}), Primitive::i32}).fingerprint());
    // A renamed field: different.
    EXPECT_NE(base, TypeTable({strukt("Point", {field("x", 1), field("z", 1)}), Primitive::i32}).fingerprint());
    // Another primitive: different.
    EXPECT_NE(base, TypeTable({strukt("Point", {field("x", 1), field("y", 1)}), Primitive::i64}).fingerprint());
    // A renamed type: the same, since a struct's own name is not hashed.
    EXPECT_EQ(base,
              TypeTable({strukt("Coordinate", {field("x", 1), field("y", 1)}), Primitive::i32}).fingerprint());
}

TEST(Fingerprint, EnumsHashWidthDiscriminantsAndVariantNamesButNotTheirName) {
    auto base = list().fingerprint();
    auto with = [](auto change) {
        auto t = list();
        auto ds = t.descriptors();
        change(std::get<Enum>(ds[0]));
        return TypeTable(ds).fingerprint();
    };
    EXPECT_EQ(base, with([](Enum& e) { e.name = "Chain"; }));
    EXPECT_EQ(base, with([](Enum& e) { std::swap(e.variants[0], e.variants[1]); }));
    EXPECT_NE(base, with([](Enum& e) { e.discriminant_width = 1; }));
    EXPECT_NE(base, with([](Enum& e) { e.variants[1].discriminant = 2; }));
    EXPECT_NE(base, with([](Enum& e) { e.variants[1].name = "Pair"; }));
}

TEST(Fingerprint, OpaqueVersionFolding) {
    // Above 0.x: patch and minor do not matter, major does.
    auto base = opaque_versioned(1, 2, 3).fingerprint();
    EXPECT_EQ(base, opaque_versioned(1, 2, 9).fingerprint());
    EXPECT_EQ(base, opaque_versioned(1, 5, 0).fingerprint());
    EXPECT_NE(base, opaque_versioned(2, 0, 0).fingerprint());
    // Within 0.x: patch does not matter, minor does.
    auto unstable = opaque_versioned(0, 1, 2).fingerprint();
    EXPECT_EQ(unstable, opaque_versioned(0, 1, 9).fingerprint());
    EXPECT_NE(unstable, opaque_versioned(0, 2, 0).fingerprint());
    // The stability flag keeps 0.1.z and 1.y.z apart.
    EXPECT_NE(opaque_versioned(0, 1, 0).fingerprint(), opaque_versioned(1, 1, 0).fingerprint());
}

TEST(Fingerprint, OpaqueSizeModeLibraryAndParametersAreHashed) {
    auto base = opaque_versioned(1, 2, 3);
    auto with = [&](auto change) {
        auto ds = base.descriptors();
        ds.push_back(Primitive::u8);
        change(std::get<Opaque>(ds[0]));
        return TypeTable(ds).fingerprint();
    };
    EXPECT_EQ(base.fingerprint(), with([](Opaque&) {}));
    EXPECT_NE(base.fingerprint(), with([](Opaque& o) { o.fixed_size = std::nullopt; }));
    EXPECT_NE(base.fingerprint(), with([](Opaque& o) { o.fixed_size = 7; }));
    EXPECT_NE(base.fingerprint(), with([](Opaque& o) { o.library_name = "other"; }));
    EXPECT_NE(base.fingerprint(), with([](Opaque& o) { o.type_name = "U"; }));
    EXPECT_NE(base.fingerprint(), with([](Opaque& o) { o.parameters = {ref(1)}; }));
}

TEST(Fingerprint, RecursionFingerprintsReproducibly) {
    for (auto const& table : {list(), mutual_pair(), rec_enum(), outer_cycle_with_inner(false, false),
                              outer_cycle_with_inner(true, true)}) {
        EXPECT_EQ(table.fingerprint(), table.fingerprint());
    }
}

TEST(Fingerprint, InvariantToSharing) {
    // `struct Pair { first: Inner, second: Inner }`, with one `Inner`
    // descriptor shared or two identical ones.
    TypeTable shared({
        strukt("Pair", {field("first", 1), field("second", 1)}),
        strukt("Inner", {field("a", 2), field("b", 2)}),
        Primitive::i32,
    });
    TypeTable duplicated({
        strukt("Pair", {field("first", 1), field("second", 2)}),
        strukt("Inner", {field("a", 3), field("b", 3)}),
        strukt("Inner", {field("a", 3), field("b", 3)}),
        Primitive::i32,
    });
    EXPECT_EQ(shared.fingerprint(), duplicated.fingerprint());
}

TEST(Fingerprint, DistinctStructuresHaveDistinctFingerprints) {
    std::vector<std::pair<char const*, TypeTable>> structures{
        {"point", point()},
        {"list", list()},
        {"mutual_pair", mutual_pair()},
        {"rec_enum", rec_enum()},
        {"selfref_cycle_first", outer_cycle_with_inner(false, false)},
        {"selfref_inner_first", outer_cycle_with_inner(false, true)},
        {"mutual_cycle_first", outer_cycle_with_inner(true, false)},
        {"mutual_inner_first", outer_cycle_with_inner(true, true)},
        {"map", text_to_u32()},
        {"sequence", TypeTable({Sequence{ref(1)}, Primitive::u32})},
        {"array", TypeTable({Array{ref(1), 2}, Primitive::u32})},
        {"fixed", TypeTable({Fixed{ref(1)}, Primitive::u32})},
        {"text", TypeTable({Text{}})},
        {"u32", TypeTable({Primitive::u32})},
    };
    for (std::size_t i = 0; i < structures.size(); ++i) {
        for (std::size_t j = i + 1; j < structures.size(); ++j) {
            EXPECT_NE(structures[i].second.fingerprint(), structures[j].second.fingerprint())
                << structures[i].first << " and " << structures[j].first << " must not collide";
        }
    }
}

TEST(Fingerprint, OfANonRootTypeIsItsFingerprintAsRoot) {
    auto table = text_to_u32();
    EXPECT_EQ(table.fingerprint(ref(1)), TypeTable({Text{}}).fingerprint());
    EXPECT_EQ(table.fingerprint(ref(2)), TypeTable({Primitive::u32}).fingerprint());
    EXPECT_EQ(list().fingerprint(ref(3)),
              TypeTable({strukt("Cons", {field("0", 1), field("1", 3)}), Primitive::i32,
                         strukt("Unit", {}),
                         Enum{"List", 4, {variant(0, "Nil", 2), variant(1, "Cons", 0)}}})
                  .fingerprint());
}

TEST(Fingerprint, ThrowsOnAReferenceOutOfRange) {
    TypeTable table({Sequence{ref(3)}});
    EXPECT_THROW((void)table.fingerprint(), std::out_of_range);
}

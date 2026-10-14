// Writes, or checks, the file of the cross-implementation test: kladde-cpp
// and kladde-rs each write one and read the other's
// (kladde-rs/crates/kladde-types/examples/cross_impl.rs holds the same types
// in Rust).
//
//     kladde-cross write drawing.kladde
//     kladde-cross check drawing.kladde
#include <cstdio>
#include <cstring>

#include <kladde/kladde.hpp>

namespace cross {

struct Point {
    float x = 0, y = 0;
    friend bool operator==(Point const&, Point const&) = default;
};
KLADDE_STRUCT(Point, x, y)

// Rust's `Circle { r: f32 }`: a variant of named fields holds the struct of
// them, named after the variant.
struct Circle {
    float r = 0;
    friend bool operator==(Circle const&, Circle const&) = default;
};
KLADDE_STRUCT(Circle, r)

// `Poly(Vec<Point>)`, `Label(String)` and `Empty` hold their one field, or
// nothing.
using Shape = std::variant<Circle, std::vector<Point>, std::string, kladde::Unit>;

struct Drawing {
    std::string name;
    kladde::Vector<Shape> shapes{};
    kladde::Map<std::string, std::uint32_t> tags{};
    std::optional<Point> origin{};
    std::uint16_t scale = 1;
};
KLADDE_STRUCT(Drawing, name, shapes, tags, origin, scale)

}  // namespace cross

// The variants' names are Rust's, which the fingerprint covers.
KLADDE_VARIANT_NAMED(cross::Shape, "Shape", (0, "Circle"), (1, "Poly"), (2, "Label"), (3, "Empty"))

namespace {

using namespace cross;

int write(char const* path) {
    auto file = kladde::File<Drawing>::create(path, Drawing{.name = "cross"});
    if (!file) {
        std::fprintf(stderr, "%s\n", file.error().what().c_str());
        return 1;
    }
    auto& d = *file;
    {
        auto m = d.edit();
        m.name().append(" implementation");
        m.shapes().push_back(Circle{1.5f});
        m.shapes().push_back(std::vector<Point>{{0, 0}, {3, 4}});
        m.shapes().push_back(std::string("ünïcödé"));
        m.shapes().push_back(kladde::Unit{});
        m.shapes().move(3, 0);
        m.tags().insert("layer", 7);
        m.tags().insert("z", 70000);
        m.origin().emplace(Point{-1, 2.5f});
        m.scale().set(300);
    }
    d.flush();
    // Left in the journal, for the reader to recover.
    d.edit().tags().remove("layer");
    return 0;
}

int fail(char const* what) {
    std::fprintf(stderr, "mismatch: %s\n", what);
    return 1;
}

int check(char const* path) {
    auto file = kladde::File<Drawing>::open(path);
    if (!file) {
        std::fprintf(stderr, "%s\n", file.error().what().c_str());
        return 1;
    }
    auto const& d = file->get();
    if (d.name != "cross implementation") return fail("name");
    std::vector<Shape> shapes{kladde::Unit{}, Circle{1.5f}, std::vector<Point>{{0, 0}, {3, 4}},
                              std::string("ünïcödé")};
    if (d.shapes.items() != shapes) return fail("shapes");
    if (d.tags.size() != 1 || !d.tags.get("z") || *d.tags.get("z") != 70000) return fail("tags");
    if (d.origin != Point{-1, 2.5f}) return fail("origin");
    if (d.scale != 300) return fail("scale");
    std::printf("%s reads as expected\n", path);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("fingerprint %s\n", kladde::fingerprint_of<Drawing>().to_hex().c_str());
    if (argc == 3 && std::strcmp(argv[1], "write") == 0) return write(argv[2]);
    if (argc == 3 && std::strcmp(argv[1], "check") == 0) return check(argv[2]);
    std::fprintf(stderr, "usage: kladde-cross (write | check) <file>\n");
    return 2;
}

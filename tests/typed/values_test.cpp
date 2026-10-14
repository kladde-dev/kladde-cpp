// Values against their canonical encoding after every operation, through
// a `VectorBackend`.
#include "fixture.hpp"

namespace app {

struct Point {
    std::int32_t x = 0, y = 0;
    friend bool operator==(Point const&, Point const&) = default;
};
KLADDE_STRUCT(Point, x, y)

enum class Join : std::uint8_t { miter, round, bevel = 7 };
KLADDE_ENUM(Join, miter, round, bevel)

struct Mobile {
    std::string digits;
    friend bool operator==(Mobile const&, Mobile const&) = default;
};
struct Landline {
    std::string digits;
    friend bool operator==(Landline const&, Landline const&) = default;
};
KLADDE_STRUCT(Mobile, digits)
KLADDE_STRUCT(Landline, digits)
using PhoneNumber = std::variant<Mobile, Landline>;

struct Contact {
    std::string email;
    kladde::Vector<PhoneNumber> phones{};
    bool starred = false;
    std::optional<Point> home{};
    int cache = 0;  // not persisted
    friend bool operator==(Contact const& a, Contact const& b) {
        return a.email == b.email && a.phones == b.phones && a.starred == b.starred && a.home == b.home;
    }
};
KLADDE_STRUCT(Contact, email, phones, starred, home)

struct Text {
    std::string content;
    friend bool operator==(Text const&, Text const&) = default;
};
KLADDE_TRANSPARENT(Text, content)

struct Element;
using Node = std::variant<kladde::Box<Element>, Text>;
struct Element {
    std::string name;
    kladde::Vector<std::pair<std::string, std::string>> attributes{};
    kladde::List<Node> children{};
    friend bool operator==(Element const&, Element const&) = default;
};
KLADDE_STRUCT(Element, name, attributes, children)

}  // namespace app

KLADDE_VARIANT(app::PhoneNumber, "PhoneNumber", 1, 2)

using kladde::testing::Fixture;

TEST(Scalars, EncodeAsTheSpecificationSays) {
    EXPECT_EQ(kladde::to_bytes(std::uint32_t{300}), (std::vector<std::uint8_t>{0xac, 0x02}));
    EXPECT_EQ(kladde::to_bytes(std::int32_t{-1}), (std::vector<std::uint8_t>{0x01}));
    EXPECT_EQ(kladde::to_bytes(true), (std::vector<std::uint8_t>{1}));
    EXPECT_EQ(kladde::to_bytes(1.5f), (std::vector<std::uint8_t>{0, 0, 0xc0, 0x3f}));
    EXPECT_EQ(kladde::to_bytes(U'é'), (std::vector<std::uint8_t>{0xc3, 0xa9}));
    EXPECT_EQ(kladde::to_bytes(std::string("ab")), (std::vector<std::uint8_t>{'a', 'b', 0xff}));
    EXPECT_FALSE(kladde::from_bytes<bool>(std::vector<std::uint8_t>{2}).has_value());
    EXPECT_EQ(kladde::from_bytes<std::int64_t>(kladde::to_bytes(std::int64_t{-12345})).value(), -12345);
}

TEST(Fingerprints, MatchTheSpecificationsVectors) {
    EXPECT_EQ(kladde::fingerprint_of<app::Point>().to_hex(), "0d91791f5827efadfa8150af9812fad6");
    EXPECT_EQ((kladde::fingerprint_of<kladde::Map<std::string, std::uint32_t>>().to_hex()),
              "6a33dc4a1e560e465f7a50543f62c492");
}

TEST(Fingerprints, AListIsAVectorOnFile) {
    EXPECT_EQ(kladde::fingerprint_of<kladde::List<app::Point>>(),
              kladde::fingerprint_of<kladde::Vector<app::Point>>());
    using P = std::pair<float, float>;
    EXPECT_EQ(kladde::fingerprint_of<std::vector<P>>(), kladde::fingerprint_of<kladde::Vector<P>>());
}

TEST(Structs, AccessorsAndPartsRecordWhatChanged) {
    Fixture<app::Contact> f(app::Contact{.email = "ada@example.com"});
    auto c = f.edit();
    c.starred().set(true);
    c.email().insert(0, "lady.");
    c.phones().push_back(app::Mobile{"555-0100"});
    c.home().emplace(app::Point{3, -4});
    f.check();
    {
        auto [email, phones, starred, home] = c.parts();
        email.append(".uk");  // `phones` and `starred` move
        phones.at(0).visit([](auto alt) { alt.digits().append("1"); });
        starred.set(false);
        home.value().x().set(70000);
    }
    f.check();
    EXPECT_EQ(f.value.email, "lady.ada@example.com.uk");
    EXPECT_EQ(std::get<app::Mobile>(f.value.phones[0]).digits, "555-01001");
    EXPECT_EQ(f.value.home->x, 70000);
}

TEST(Text, SetRecordsOnlyTheChangedMiddle) {
    Fixture<std::string> f(std::string(1000, 'a'));
    auto t = f.edit();
    auto before = f.bytes.bytes();
    std::string changed(1000, 'a');
    changed[500] = 'b';
    t.set(changed);
    f.check();
    t.replace(10, 3, "xyz");
    t.erase(0, 2);
    f.check();
#if KLADDE_EXCEPTIONS
    EXPECT_THROW(t.append("\xff"), kladde::exception);
    EXPECT_THROW(t.insert(5000, "x"), kladde::exception);
#endif
    f.check();
}

TEST(Vector, InsertsRemovesMovesAndCuts) {
    using V = kladde::Vector<std::string>;
    struct Two {
        V a, b;
    };
    Fixture<std::pair<V, V>> f({V{"circle", "square", "triangle"}, V{}});
    auto pair = f.edit();
    auto [a, b] = pair.parts();
    a.push_back("hexagon");
    a.move(3, 0);
    EXPECT_EQ(*a.remove(1), "circle");
    a.at(0).append("!");
    auto cut = a.cut(1);
    ASSERT_TRUE(cut);
    EXPECT_EQ(**cut, "square");
    b.push_back_cut(std::move(*cut));
    f.check();
    f.value.first.check();
    f.value.second.check();
    EXPECT_EQ(f.value.first.items(), (std::vector<std::string>{"hexagon!", "triangle"}));
    EXPECT_EQ(f.value.second.items(), (std::vector<std::string>{"square"}));
}

TEST(StdVector, HoldsFixedSizeElements) {
    Fixture<std::vector<float>> f({1, 2});
    auto v = f.edit();
    v.push_back(3);
    v.at(0).set(10);
    v.erase(1);
    f.check();
    EXPECT_EQ(f.value, (std::vector<float>{10, 3}));
}

TEST(Map, InsertsReplacesAndRemovesByMovingTheLast) {
    Fixture<kladde::Map<std::string, std::uint32_t>> f({});
    auto m = f.edit();
    m.insert("Emma", 3);
    m.insert("Persuasion", 1);
    m.insert("Sanditon", 300);
    m.at("Emma")->set(70000);
    EXPECT_EQ(m.remove("Emma"), 70000u);
    f.check();
    EXPECT_EQ(f.value.entries().front().first, "Sanditon");
    EXPECT_EQ(*f.value.get("Persuasion"), 1u);
}

TEST(Enums, ScalarEnumsAndVariants) {
    Fixture<std::tuple<app::Join, app::PhoneNumber, std::optional<std::int8_t>>> f(
        {app::Join::round, app::Landline{"1"}, std::nullopt});
    auto t = f.edit();
    t.get<0>().set(app::Join::bevel);
    t.get<1>().set(app::Mobile{"2"});
    t.get<2>().emplace(-3);
    f.check();
    auto table = kladde::schema_of<app::PhoneNumber>();
    auto const& e = std::get<kladde::schema::Enum>(table.get(table.root()));
    EXPECT_EQ(e.name, "PhoneNumber");
    EXPECT_EQ(e.variants[0].discriminant, 1u);
    EXPECT_EQ(e.variants[0].name, "Mobile");
    EXPECT_EQ(kladde::to_bytes(app::PhoneNumber(app::Mobile{"x"})),
              (std::vector<std::uint8_t>{1, 'x', 0xff}));
}

TEST(List, RefsSurviveEditsElsewhere) {
    app::Element root{.name = "svg"};
    Fixture<app::Element> f(root);
    auto r = f.edit();
    r.children().push_back(app::Text{"hello"});
    r.children().push_back(kladde::Box<app::Element>(app::Element{.name = "g"}));
    f.check();
    auto hello = f.value.children.ref(0);
    auto g = f.value.children.ref(1);
    // Grow the group through its own handle, then edit the text before it.
    g.edit().visit([](auto alt) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(alt.get())>, app::Element>) {
            alt.children().push_back(app::Text{"inner"});
            alt.attributes().push_back({"id", "layer1"});
        }
    });
    f.check();
    hello.edit().visit([](auto alt) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(alt.get())>, app::Text>)
            alt.set("hello, world");
    });
    f.edit().name().set("svg:svg");
    f.check();
    // A nested element's own ref, after its parent and an earlier sibling grew.
    auto& gel = *std::get<kladde::Box<app::Element>>(f.value.children[1]);
    auto inner = gel.children.ref(0);
    inner.edit().visit([](auto alt) {
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(alt.get())>, app::Text>)
            alt.set("inner text that is longer");
    });
    f.check();
    f.value.children.check();
    gel.children.check();
    // Detach the text and insert it again elsewhere, at its old address.
    auto e = f.edit().children().detach(0);
    ASSERT_TRUE(e);
    auto* addr = e.get();
    EXPECT_FALSE(hello.attached());
    auto children = f.edit();
    {
        auto [name, attrs, kids] = children.parts();
        (void)name;
        (void)attrs;
        kids.insert_element(1, std::move(e));
    }
    EXPECT_EQ(f.value.children.element_at(1), addr);
    EXPECT_TRUE(hello.attached());
    EXPECT_EQ(hello.index(), 1u);
    f.check();
}

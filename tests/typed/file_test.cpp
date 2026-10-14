// `kladde::File` over the store: values survive reopening, transactions are
// all or nothing, and long-lived refs keep working across flushes.
#include "fixture.hpp"

namespace doc {

struct Text {
    std::string content;
    friend bool operator==(Text const&, Text const&) = default;
};
KLADDE_STRUCT(Text, content)

struct Element;
using Node = std::variant<kladde::Box<Element>, Text>;
struct Element {
    std::string name;
    kladde::Vector<std::pair<std::string, std::string>> attributes{};
    kladde::List<Node> children{};
    friend bool operator==(Element const&, Element const&) = default;
};
KLADDE_STRUCT(Element, name, attributes, children)

}  // namespace doc

namespace {

template <class T>
kladde::File<T> reopen(kladde::MemoryStorage const& storage) {
    auto image = kladde::MemoryStorage::from_image(storage.image());
    auto f = kladde::File<T>::open_in(std::make_unique<kladde::MemoryStorage>(image));
    EXPECT_TRUE(f.has_value()) << (f ? "" : f.error().what());
    return std::move(*f);
}

kladde::Options tiny() {
    kladde::Options o;
    o.journal_budget_pages = 1;
    return o;
}

}  // namespace

TEST(File, AValueSurvivesReopeningWithAndWithoutAFlush) {
    kladde::MemoryStorage storage;
    auto f = kladde::File<doc::Element>::create_in(std::make_unique<kladde::MemoryStorage>(storage),
                                                   doc::Element{.name = "svg"})
                 .value();
    auto e = f.edit();
    e.children().push_back(doc::Text{"hello"});
    e.attributes().push_back({"width", "100"});
    EXPECT_EQ(reopen<doc::Element>(storage).get(), f.get());  // from the journal
    f.flush();
    EXPECT_EQ(reopen<doc::Element>(storage).get(), f.get());  // from the file
}

TEST(File, OpeningAsAnotherTypeFailsWithASchemaMismatch) {
    kladde::MemoryStorage storage;
    (void)kladde::File<std::uint32_t>::create_in(std::make_unique<kladde::MemoryStorage>(storage), 7).value();
    auto image = kladde::MemoryStorage::from_image(storage.image());
    auto f = kladde::File<std::string>::open_in(std::make_unique<kladde::MemoryStorage>(image));
    ASSERT_FALSE(f.has_value());
    EXPECT_EQ(f.error().code, kladde::errc::schema_mismatch);
}

TEST(File, RefsKeepWorkingAcrossManyFlushes) {
    kladde::MemoryStorage storage;
    auto f = kladde::File<doc::Element>::create_in(std::make_unique<kladde::MemoryStorage>(storage),
                                                   doc::Element{.name = "svg"}, tiny())
                 .value();
    for (int i = 0; i < 20; ++i) {
        f.edit().children().push_back(kladde::Box<doc::Element>(doc::Element{.name = "g" + std::to_string(i)}));
    }
    std::vector<kladde::Ref<doc::Node>> groups;
    for (std::size_t i = 0; i < 20; ++i) groups.push_back(f.get().children.ref(i));
    // Many edits deep in the tree, each through a ref, with flushes between
    // them since the journal holds one page.
    for (int round = 0; round < 30; ++round) {
        for (std::size_t i = 0; i < groups.size(); i += 3) {
            groups[i].edit().if_holds<kladde::Box<doc::Element>>([&](kladde::Mut<kladde::Box<doc::Element>> g) {
                g.attributes().push_back({"round", std::to_string(round)});
                g.children().push_back(doc::Text{std::string(static_cast<std::size_t>(round), 'x')});
            });
        }
        f.edit().name().append("!");
    }
    EXPECT_GE(f.stats().flushes, 3u);  // the one at creation, and those the journal forced
    f.get().children.check();
    EXPECT_EQ(reopen<doc::Element>(storage).get(), f.get());
    f.close();
    EXPECT_EQ(reopen<doc::Element>(storage).get(), f.get());
}

TEST(File, ATransactionThatThrowsKeepsNothingAndPoisons) {
    using Accounts = std::pair<std::uint32_t, std::uint32_t>;
    kladde::MemoryStorage storage;
    auto f = kladde::File<Accounts>::create_in(std::make_unique<kladde::MemoryStorage>(storage), {100, 0})
                 .value();
    f.transaction([](auto both) {
        auto [from, to] = both.parts();
        from.set(70);
        to.set(30);
    });
    EXPECT_EQ(reopen<Accounts>(storage).get(), Accounts(70, 30));
#if KLADDE_EXCEPTIONS
    EXPECT_THROW(f.transaction([](auto both) {
        both.template get<0>().set(1);
        throw std::runtime_error("mid-transaction");
    }),
                 std::runtime_error);
    EXPECT_TRUE(f.is_poisoned());
    EXPECT_EQ(reopen<Accounts>(storage).get(), Accounts(70, 30));
#endif
}

TEST(File, IdleFlushOnlyPastTheFraction) {
    auto f = kladde::File<std::string>::create_in(std::make_unique<kladde::MemoryStorage>(), "").value();
    f.edit().append("a few bytes");
    EXPECT_FALSE(f.flush_if_journal_exceeds(0.5));
    EXPECT_TRUE(f.flush_if_journal_exceeds(0.0));
}

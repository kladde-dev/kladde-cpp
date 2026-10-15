// Unit tests of the journal: records and their encoding, the buffer, the
// chain of journal pages, and recovery.
#include <gtest/gtest.h>

#include "../../src/store/journal.hpp"
#include "../../src/store/util.hpp"
#include "test_util.hpp"

using namespace kladde::store;
using kladde::MemoryStorage;
using kladde::Storage;
using kladde::expected;
using kladde::test::bytes;

namespace {

MemoryStorage setup(std::uint64_t pages) {
    MemoryStorage s;
    EXPECT_TRUE(s.resize(pages * PAGE_SIZE));
    return s;
}

// Decodes a transaction that names no payload page.
expected<bool> decode_inline(std::span<std::uint8_t const> input, std::uint64_t& cursor,
                             DecodedCuts& cuts, Records& into) {
    return decode_records(input, cursor, cuts, into, [](std::uint64_t, std::uint32_t, std::uint32_t)
                              -> expected<std::optional<std::pair<std::vector<std::uint8_t>, Placement>>> {
        return corrupt("no payload pages here");
    });
}

struct Rng {
    std::uint64_t state;
    std::uint64_t next(std::uint32_t bits) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state % (std::uint64_t{1} << std::min<std::uint32_t>(bits, 48));
    }
};

/// A transaction of random records, with valid cuts and pastes; `cuts` are
/// the journal's, which a paste may name whichever transaction made them.
void transaction(Rng& rng, Records& recs, DecodedCuts& cuts) {
    std::uint64_t start = recs.end().at;
    std::uint64_t count = rng.next(4) + 1;
    for (std::uint64_t k = 0; k < count; ++k) {
        auto bits = static_cast<std::uint32_t>(rng.next(5) + 1);
        std::uint64_t at = recs.end().at;
        Record r;
        switch (rng.next(3)) {
            case 0: {
                std::size_t n = rng.next(8) + 1;
                Bytes b = recs.stash(std::vector<std::uint8_t>(n, static_cast<std::uint8_t>(rng.next(8))));
                cuts.push_back({at, n, false});
                r = Record::write(rng.next(bits), b);
                break;
            }
            case 1: {
                std::size_t n = rng.next(8);
                Bytes b = recs.stash(std::vector<std::uint8_t>(n, static_cast<std::uint8_t>(rng.next(8))));
                std::uint64_t removed = rng.next(bits);
                if (removed > 0) cuts.push_back({at, removed, false});
                r = Record::splice(rng.next(bits), removed, b);
                break;
            }
            case 2: {
                std::uint64_t from = rng.next(bits);
                std::uint64_t len = rng.next(bits);
                r = Record::move(from, len, rng.next(bits));
                break;
            }
            default: {
                std::optional<std::size_t> found;
                for (std::size_t i = cuts.size(); i-- > 0;) {
                    if (!cuts[i].pasted) {
                        found = i;
                        break;
                    }
                }
                if (found) {
                    cuts[*found].pasted = true;
                    std::uint64_t pos = rng.next(bits);
                    std::uint64_t removed = rng.next(bits);
                    r = Record::paste(pos, removed, cuts[*found].record, cuts[*found].len, std::nullopt);
                } else {
                    r = Record::move(1, 2, 3);
                }
            }
        }
        recs.push(r);
    }
    recs.mark_start(start);
}

/// Each record with its payload, for comparing buffers, the payload's
/// position in the arena left out.
std::vector<std::pair<Record, std::vector<std::uint8_t>>> contents(Records const& recs) {
    std::vector<std::pair<Record, std::vector<std::uint8_t>>> out;
    for (Record r : recs.all()) {
        std::vector<std::uint8_t> payload;
        if (r.kind == Record::Kind::write || r.kind == Record::Kind::splice) {
            payload = recs.to_vec(r.bytes);
            r.bytes.start = 0;
        }
        out.emplace_back(r, payload);
    }
    return out;
}

std::vector<std::uint8_t> put(std::initializer_list<std::uint64_t> values) {
    std::vector<std::uint8_t> out;
    for (std::uint64_t v : values) kladde::put_varint(out, v);
    return out;
}

bool refused(std::vector<std::uint8_t> const& out) {
    std::uint64_t cursor = 0;
    DecodedCuts cuts;
    Records recs;
    return !decode_inline(out, cursor, cuts, recs);
}

NextPage growing_allocator(std::uint32_t& next, bool grow) {
    return [&next, grow](Storage& s) -> expected<std::uint32_t> {
        std::uint32_t p = next++;
        if (grow) KLADDE_TRY(s.resize((std::uint64_t{p} + 1) * PAGE_SIZE));
        return p;
    };
}

}  // namespace

TEST(Journal, RecordsRoundTripAndRecordLenCountsTheEncoding) {
    Rng rng{0x9E3779B97F4A7C15};
    Records recs;
    std::vector<std::pair<Cursor, Cursor>> txs;
    DecodedCuts cuts;
    for (int k = 0; k < 500; ++k) {
        Cursor from = recs.end();
        transaction(rng, recs, cuts);
        txs.emplace_back(from, recs.end());
    }
    std::uint64_t cursor = 0;
    JournalCuts journal = JournalCuts::starting_at(0);
    Records decoded;
    std::uint64_t dcursor = 0;
    DecodedCuts dcuts;
    std::uint64_t total = 0;
    for (auto [from, to] : txs) {
        std::vector<std::uint8_t> out;
        recs.encode(from, to, cursor, journal, out);
        // The estimate assumed the same cursor, and a paste's distance at most
        // the records since its cut, so it never falls short.
        EXPECT_GE(to.bytes - from.bytes, out.size());
        total += out.size();
        std::uint64_t at = decoded.end().at;
        ASSERT_TRUE(decode_inline(out, dcursor, dcuts, decoded));
        decoded.mark_start(at);
    }
    EXPECT_LE(total, recs.end().bytes);
    EXPECT_EQ(contents(decoded), contents(recs));
    EXPECT_EQ(dcursor, cursor);
}

TEST(Journal, AKeystrokeTakesFourBytes) {
    Records recs;
    Bytes b = recs.stash(bytes("a"));
    Record r = Record::splice(1000, 0, b);
    EXPECT_EQ(record_len(r, 1000, 0), 4u);
    EXPECT_EQ(record_len(r, 999, 0), 4u);
    EXPECT_GT(record_len(r, 0, 0), 4u);
}

TEST(Journal, PastesOfUnknownOrPastedCutsAndTheEscapeAreRefused) {
    // A paste in a journal that cut nothing.
    EXPECT_TRUE(refused(put({zz(3), 0, 0})));
    // A write of no bytes.
    EXPECT_TRUE(refused(put({zz(0), 0})));
    // A cut pasted twice.
    EXPECT_TRUE(refused(put({zz(1), 2, 0, zz(3), 0, 0, zz(3), 0, 0})));
}

TEST(Journal, APasteNamesACutOfAnEarlierTransactionOfItsJournal) {
    // A splice that removes two bytes, then, in another transaction, a paste
    // of the cut it made.
    auto first = put({zz(1), 2, 0});
    auto second = put({zz(3), 0, 0});
    std::uint64_t cursor = 0;
    DecodedCuts cuts;
    Records recs;
    ASSERT_TRUE(decode_inline(first, cursor, cuts, recs));
    ASSERT_TRUE(decode_inline(second, cursor, cuts, recs));
    EXPECT_EQ(recs.all().at(1), Record::paste(0, 0, 0, 2, std::nullopt));
}

TEST(Journal, APasteWhoseCutHasEndedIsWrittenAsItsFallback) {
    Records recs;
    Bytes gone = recs.stash({});
    recs.push(Record::splice(0, 2, gone));
    Bytes fallback = recs.stash(bytes("xy"));
    recs.push(Record::paste(3, 0, 0, 2, fallback));
    Record paste = recs.all().at(1);
    // In the journal of its cut, it is a paste; in the next, a splice.
    EXPECT_EQ(paste.as_written(0).kind, Record::Kind::paste);
    EXPECT_EQ(paste.as_written(1), Record::splice(3, 0, fallback));
}

TEST(Journal, AWriteMakesACutOfItsLength) {
    // A write of two bytes, then a paste of the cut it made.
    std::vector<std::uint8_t> out = put({zz(0), 2});
    out.push_back('x');
    out.push_back('y');
    auto rest = put({zz(3 - (2 << 2)), 0, 0});
    out.insert(out.end(), rest.begin(), rest.end());
    std::uint64_t cursor = 0;
    DecodedCuts cuts;
    Records recs;
    ASSERT_TRUE(decode_inline(out, cursor, cuts, recs));
    EXPECT_EQ(recs.all().at(1), Record::paste(0, 0, 0, 2, std::nullopt));
}

TEST(Journal, TheBufferKeepsPositionsAndStartsAsItsFrontMoves) {
    Rng rng{0x2545F4914F6CDD1D};
    Records recs;
    std::vector<Cursor> marks;
    DecodedCuts cuts;
    for (int k = 0; k < 40; ++k) {
        marks.push_back(recs.end());
        transaction(rng, recs, cuts);
    }
    auto all = contents(recs);
    Cursor cut = marks[15];
    recs.discard_to(cut);
    std::vector<std::uint64_t> starts;
    for (std::uint64_t at = recs.front().at; at < recs.end().at; ++at)
        if (recs.is_start(at)) starts.push_back(at);
    std::vector<std::uint64_t> want;
    for (std::size_t k = 15; k < marks.size(); ++k) want.push_back(marks[k].at);
    EXPECT_EQ(starts, want);
    EXPECT_EQ(contents(recs), decltype(all)(all.begin() + static_cast<std::ptrdiff_t>(cut.at), all.end()));
    recs.truncate(marks[30]);
    EXPECT_EQ(recs.end(), marks[30]);
    std::size_t count = 0;
    for (std::uint64_t at = recs.front().at; at < recs.end().at; ++at) count += recs.is_start(at);
    EXPECT_EQ(count, 15u);
}

TEST(Journal, TransactionsAcrossManyPagesRoundTrip) {
    MemoryStorage storage = setup(3);
    JournalWriter w(8, 2);
    std::uint32_t next = 3;
    auto alloc = growing_allocator(next, true);
    Rng rng{77};
    Records recs;
    DecodedCuts cuts;
    JournalCuts journal = JournalCuts::starting_at(0);
    for (int k = 0; k < 200; ++k) {
        Cursor from = recs.end();
        transaction(rng, recs, cuts);
        // Payloads large enough to cross pages.
        Bytes b = recs.stash(std::vector<std::uint8_t>(300, 7));
        cuts.push_back({recs.end().at, 300, false});
        recs.push(Record::write(5, b));
        std::vector<std::uint8_t> payload;
        std::uint64_t c = w.cursor;
        recs.encode(from, recs.end(), c, journal, payload);
        w.cursor = c;
        ASSERT_TRUE(w.append(payload, storage, alloc));
    }
    EXPECT_GT(w.pages.size(), 3u);
    auto got = recover(storage, 8, 2);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->transactions, 200u);
    EXPECT_EQ(contents(got->records), contents(recs));
    // A different epoch salt validates nothing.
    EXPECT_EQ(recover(storage, 9, 2)->transactions, 0u);
}

TEST(Journal, ARecoveredJournalNeverFitsAnything) {
    JournalWriter w = JournalWriter::recovered(1, {2, 3}, {}, 4);
    EXPECT_FALSE(w.is_empty());
    EXPECT_FALSE(w.fits(0, UINT32_MAX));
}

TEST(Journal, ATornTailKeepsTheLongestValidPrefixAndSkipsNoHole) {
    MemoryStorage storage = MemoryStorage::with_crash_tracking();
    ASSERT_TRUE(storage.resize(6 * PAGE_SIZE));
    ASSERT_TRUE(storage.sync());
    JournalWriter w(3, 2);
    std::uint32_t next = 3;
    auto alloc = growing_allocator(next, false);
    Records recs;
    for (std::size_t t = 0; t < 8; ++t) {
        Cursor from = recs.end();
        Bytes b = recs.stash(std::vector<std::uint8_t>(1000 + t, static_cast<std::uint8_t>(t)));
        recs.push(Record::write(0, b));
        std::vector<std::uint8_t> payload;
        std::uint64_t c = 0;
        JournalCuts journal = JournalCuts::starting_at(0);
        recs.encode(from, recs.end(), c, journal, payload);
        ASSERT_TRUE(w.append(payload, storage, alloc));
    }
    std::size_t ops = storage.unsynced_writes();
    // Losing any single write leaves exactly the transactions before it.
    for (std::size_t lost = 0; lost < ops; ++lost) {
        auto img = storage.crash_image([&](std::size_t i) { return i != lost; });
        auto got = recover(MemoryStorage::from_image(img), 3, 2);
        ASSERT_TRUE(got);
        EXPECT_LT(got->transactions, 8u);
        auto prefix_img = storage.crash_image([&](std::size_t i) { return i < lost; });
        auto prefix = recover(MemoryStorage::from_image(prefix_img), 3, 2);
        ASSERT_TRUE(prefix);
        EXPECT_EQ(got->transactions, prefix->transactions) << "losing write " << lost;
    }
}

TEST(Journal, RecoveryFollowsASealIntoTheNextEpochsJournal) {
    MemoryStorage storage = setup(6);
    std::uint32_t next = 4;
    auto alloc = growing_allocator(next, false);
    auto write = [](std::uint64_t pos, char c) {
        std::vector<std::uint8_t> out = put({zz(static_cast<std::int64_t>(pos) << 2), 1});
        out.push_back(static_cast<std::uint8_t>(c));
        return out;
    };
    JournalWriter first(5, 2);
    ASSERT_TRUE(first.append(write(0, 'a'), storage, alloc));
    ASSERT_TRUE(first.append(seal_payload(3), storage, alloc));
    // The next journal: epoch 6, cursor 0 again.
    JournalWriter second(6, 3);
    ASSERT_TRUE(second.append(write(1, 'b'), storage, alloc));
    auto got = recover(storage, 5, 2);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->transactions, 3u);
    EXPECT_EQ(got->pages, (std::vector<std::uint32_t>{2, 3}));
    ASSERT_EQ(got->records.all().size(), 2u);
    EXPECT_EQ(got->records.all()[1].pos, 1u);
    EXPECT_EQ(parse_seal(seal_payload(300)), 300u);
    // Recovery from the next journal's own header reads it alone.
    EXPECT_EQ(recover(storage, 6, 3)->transactions, 1u);
}

TEST(Journal, ASealWithMoreRecordsIsRefused) {
    auto payload = seal_payload(3);
    payload.push_back(0);
    EXPECT_FALSE(parse_seal(payload));
    EXPECT_TRUE(refused(payload));
}

TEST(Journal, AJournalPointerPastTheFileIsAnEmptyJournal) {
    MemoryStorage storage = setup(2);
    auto got = recover(storage, 1, 2);
    ASSERT_TRUE(got);
    EXPECT_EQ(got->transactions, 0u);
}

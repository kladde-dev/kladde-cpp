#include "entries.hpp"

#include <algorithm>
#include <string>

#include "consts.hpp"
#include "util.hpp"

namespace kladde::store {

namespace {

/// Encodes one entry after an entry of id `prev_id` and a reference that
/// ended at `prev_end`; returns where an inline payload starts.
std::size_t encode_one(EntryOut const& e, std::uint32_t prev_id, std::uint64_t prev_end,
                       std::vector<std::uint8_t>& out) {
    KLADDE_DEBUG_CHECK(e.id > prev_id, "entries out of order");
    put_varint(out, e.id - prev_id);
    bool implicit = e.next != 0 && e.next == e.id + 1u;
    auto bit = static_cast<std::uint8_t>(implicit);
    std::size_t payload = 0;
    switch (e.body.kind) {
        case Body::Kind::ref:
            out.push_back(bit);
            put_varint(out, e.body.len);
            put_varint(out, zz(static_cast<std::int64_t>(e.body.addr) -
                               static_cast<std::int64_t>(prev_end)));
            break;
        case Body::Kind::zero:
            out.push_back(static_cast<std::uint8_t>(2 | bit));
            put_varint(out, e.body.len);
            break;
        case Body::Kind::inline_:
            KLADDE_DEBUG_CHECK(!e.body.payload.empty() && e.body.payload.size() <= MAX_INLINE,
                               "an inline payload out of bounds");
            out.push_back(static_cast<std::uint8_t>(((e.body.payload.size() + 1) << 1) | bit));
            payload = out.size();
            out.insert(out.end(), e.body.payload.begin(), e.body.payload.end());
            break;
    }
    if (!implicit)
        put_varint(out, e.next == 0 ? 0
                                    : zz(static_cast<std::int64_t>(e.next) -
                                         static_cast<std::int64_t>(e.id)));
    return payload;
}

}  // namespace

std::size_t children_len(std::span<std::uint32_t const> children) {
    std::uint32_t prev = 0;
    std::size_t n = 1;
    for (std::uint32_t c : children) {
        n += varint_len(c - prev);
        prev = c;
    }
    return n;
}

std::vector<Placed> encode_entries(std::span<std::uint32_t const> children,
                                   std::span<EntryOut const> entries,
                                   std::vector<std::uint8_t>& out) {
    std::uint32_t prev = 0;
    for (std::uint32_t c : children) {
        KLADDE_DEBUG_CHECK(c > prev, "child references out of order");
        put_varint(out, c - prev);
        prev = c;
    }
    out.push_back(0);
    std::vector<Placed> placed;
    placed.reserve(entries.size());
    std::uint32_t prev_id = 0;
    std::uint64_t prev_end = 0;
    for (EntryOut const& e : entries) {
        std::size_t start = out.size();
        std::size_t payload = encode_one(e, prev_id, prev_end, out);
        prev_id = e.id;
        if (e.body.kind == Body::Kind::ref) prev_end = e.body.addr + e.body.len;
        placed.push_back({e.id, static_cast<std::uint16_t>(out.size() - start), payload});
    }
    return placed;
}

PageEncoder::PageEncoder(std::span<std::uint32_t const> children, std::size_t capacity)
    : capacity_(capacity) {
    out.reserve(capacity);
    encode_entries(children, {}, out);
}

bool PageEncoder::push(EntryOut const& e) {
    std::size_t start = out.size();
    // Encoded as if after the previous entry: a one-entry run with the
    // cursors carried over.
    std::size_t payload = encode_one(e, prev_id_, prev_end_, out);
    if (out.size() > capacity_) {
        out.resize(start);
        return false;
    }
    prev_id_ = e.id;
    if (e.body.kind == Body::Kind::ref) prev_end_ = e.body.addr + e.body.len;
    placed.push_back({e.id, static_cast<std::uint16_t>(out.size() - start), payload});
    return true;
}

std::size_t estimate(std::uint32_t id, std::uint32_t next, Body const& body) {
    std::size_t next_len;
    if (next != 0 && next == id + 1u)
        next_len = 0;
    else if (next == 0)
        next_len = 1;
    else
        next_len = std::min<std::size_t>(
            varint_len(zz(static_cast<std::int64_t>(next) - static_cast<std::int64_t>(id))), 3);
    std::size_t body_len = 0;
    switch (body.kind) {
        case Body::Kind::ref: body_len = varint_len(body.len) + 2; break;
        case Body::Kind::zero: body_len = varint_len(body.len); break;
        case Body::Kind::inline_: body_len = body.payload.size(); break;
    }
    return 2 + next_len + body_len;
}

expected<Decoded> decode_entries(std::span<std::uint8_t const> content) {
    std::span<std::uint8_t const> rest = content;
    auto bad = [](char const* m) { return corrupt(std::string("a span-table page ") + m); };
    auto get = [&]() -> expected<std::uint64_t> {
        auto v = get_varint(rest);
        if (!v) return bad("is truncated");
        return *v;
    };
    Decoded out;
    std::uint64_t page = 0;
    for (;;) {
        KLADDE_TRY_ASSIGN(std::uint64_t d, get());
        if (d == 0) break;
        page += d;
        if (d >= UINT32_MAX || page >= UINT32_MAX) return bad("names a page past the file's bounds");
        out.children.push_back(static_cast<std::uint32_t>(page));
    }
    std::uint64_t id = 0;
    std::int64_t prev_end = 0;
    while (!rest.empty()) {
        std::size_t start = content.size() - rest.size();
        KLADDE_TRY_ASSIGN(std::uint64_t d, get());
        if (d == 0) return bad("holds entries out of order");
        if (d >= UINT32_MAX || id + d >= UINT32_MAX) return bad("names a span id past 2^32 - 1");
        id += d;
        if (rest.empty()) return bad("is truncated");
        std::uint8_t tag = rest[0];
        rest = rest.subspan(1);
        bool implicit = (tag & 1) == 1;
        EntryIn e;
        e.id = static_cast<std::uint32_t>(id);
        switch (tag >> 1) {
            case 0: {
                KLADDE_TRY_ASSIGN(std::uint64_t len, get());
                KLADDE_TRY_ASSIGN(std::uint64_t z, get());
                std::int64_t addr = 0;
                if (__builtin_add_overflow(prev_end, unzigzag(z), &addr) || len == 0 ||
                    len > MAX_PAGE_CONTENT || addr < 0)
                    return bad("holds a reference out of bounds");
                auto a = static_cast<std::uint64_t>(addr);
                std::uint64_t pg = a / PAGE_SIZE, off = a % PAGE_SIZE;
                if (pg < 2 || off < CONTENT_OFFSET || off + len > PAGE_SIZE - 4)
                    return bad("holds a reference outside a data page");
                prev_end = addr + static_cast<std::int64_t>(len);
                e.kind = Body::Kind::ref;
                e.len = static_cast<std::uint32_t>(len);
                e.addr = a;
                break;
            }
            case 1: {
                KLADDE_TRY_ASSIGN(std::uint64_t len, get());
                if (len == 0 || len > UINT32_MAX) return bad("holds an empty or overlong span of zeros");
                e.kind = Body::Kind::zero;
                e.len = static_cast<std::uint32_t>(len);
                break;
            }
            default: {
                std::size_t len = (tag >> 1) - 1u;
                if (rest.size() < len) return bad("is truncated");
                e.kind = Body::Kind::inline_;
                e.offset = content.size() - rest.size();
                e.len = static_cast<std::uint32_t>(len);
                rest = rest.subspan(len);
                break;
            }
        }
        std::uint64_t next = 0;
        if (implicit) {
            next = id + 1;
        } else {
            KLADDE_TRY_ASSIGN(std::uint64_t z, get());
            if (z != 0) {
                std::int64_t n = 0;
                if (__builtin_add_overflow(static_cast<std::int64_t>(id), unzigzag(z), &n) || n <= 0 ||
                    n >= static_cast<std::int64_t>(UINT32_MAX) || n == static_cast<std::int64_t>(id))
                    return bad("links a span to itself or out of bounds");
                next = static_cast<std::uint64_t>(n);
            }
        }
        if (next >= UINT32_MAX) return bad("links a span out of bounds");
        e.next = static_cast<std::uint32_t>(next);
        e.entry_len = static_cast<std::uint16_t>(content.size() - rest.size() - start);
        out.entries.push_back(e);
    }
    return out;
}

}  // namespace kladde::store

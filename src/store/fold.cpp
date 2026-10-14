#include "fold.hpp"

#include <unordered_map>

#include "treap.hpp"
#include "util.hpp"

namespace kladde::store {

namespace {

/// The two parts of `p`, `off` bytes into it.
std::pair<Piece, Piece> cut_piece(Piece const& p, std::uint64_t off) {
    return {Piece{p.arena, p.start, off}, Piece{p.arena, p.start + off, p.len - off}};
}

/// The piece table of one fold.
struct PieceTable {
    Treap<Piece> t;
    std::uint32_t root = NIL;

    std::uint64_t len() const { return t.sum(root); }

    std::pair<std::uint32_t, std::uint32_t> split(std::uint32_t r, std::uint64_t pos) {
        return t.split_at(r, pos, cut_piece);
    }

    /// Takes `[pos, pos + n)` out, and returns it as a tree of its own.
    std::uint32_t take(std::uint64_t pos, std::uint64_t n) {
        auto [a, rest] = split(root, pos);
        auto [m, b] = split(rest, n);
        root = t.merge(a, b);
        return m;
    }

    /// Inserts the tree `m` at `pos`.
    void insert(std::uint64_t pos, std::uint32_t m) {
        auto [a, b] = split(root, pos);
        std::uint32_t left = t.merge(a, m);
        root = t.merge(left, b);
    }

    void insert_piece(std::uint64_t pos, Piece p) {
        if (p.len > 0) insert(pos, t.node(p));
    }
};

error past_end() { return corrupt("a journal record reaches past the end of the sequence"); }

/// Whether `[pos, pos + n)` lies outside a sequence of `len` bytes.
bool outside(std::uint64_t pos, std::uint64_t n, std::uint64_t len) {
    return pos > len || n > len - pos;
}

}  // namespace

expected<std::vector<Piece>> fold(Records const& records, Cursor from, Cursor to,
                                  std::uint64_t committed, std::uint64_t first) {
    PieceTable pt;
    if (committed > 0) pt.root = pt.t.node(Piece{false, 0, committed});
    // The journal's cuts, by the number of the record that made each: their
    // trees, or `NIL` once pasted.
    std::unordered_map<std::uint64_t, std::uint32_t> cuts;
    auto applied = records.for_range(from, to, [&](bool, std::uint64_t at, Record const& rec) -> expected<void> {
        std::uint64_t len = pt.len();
        Record r = rec.as_written(first);
        switch (r.kind) {
            case Record::Kind::write: {
                std::uint64_t n = r.bytes.len;
                if (outside(r.pos, n, len)) return past_end();
                cuts[at] = pt.take(r.pos, n);
                pt.insert_piece(r.pos, Piece{true, r.bytes.start, n});
                break;
            }
            case Record::Kind::splice: {
                if (outside(r.pos, r.removed, len)) return past_end();
                if (r.removed > 0) cuts[at] = pt.take(r.pos, r.removed);
                pt.insert_piece(r.pos, Piece{true, r.bytes.start, r.bytes.len});
                break;
            }
            case Record::Kind::move: {
                if (outside(r.pos, r.len, len) || r.to > len - r.len) return past_end();
                std::uint32_t m = pt.take(r.pos, r.len);
                pt.insert(r.to, m);
                break;
            }
            case Record::Kind::paste: {
                if (outside(r.pos, r.removed, len)) return past_end();
                auto slot = cuts.find(r.cut);
                if (slot == cuts.end()) return corrupt("a paste names a cut its journal did not make");
                std::uint32_t m = slot->second;
                slot->second = NIL;
                if (m == NIL && r.len > 0) return corrupt("a paste names a cut already pasted");
                if (pt.t.sum(m) != r.len) return corrupt("a paste's cut has another length");
                std::uint32_t dropped = pt.take(r.pos, r.removed);
                pt.t.free_tree(dropped);
                pt.insert(r.pos, m);
                break;
            }
        }
        return {};
    });
    KLADDE_TRY(applied);
    std::vector<Piece> out;
    for (std::uint32_t n : pt.t.nodes(pt.root)) {
        Piece const& p = pt.t.item(n);
        if (!out.empty() && out.back().continued_by(p))
            out.back().len += p.len;
        else
            out.push_back(p);
    }
    return out;
}

}  // namespace kladde::store

// The fold: the journal's records applied to a piece table over the sequence,
// which starts as one piece, the committed sequence, and ends as the new
// sequence described by committed ranges and new bytes.
//
// The pieces sit in a counted tree, so each record costs O(log p) for the p
// pieces the journal made, however long the sequence; the journal's cuts are
// kept as trees of their own until the fold ends.
#pragma once

#include <cstdint>
#include <vector>

#include "journal.hpp"

namespace kladde::store {

/// A run of the new sequence and where its bytes come from: the committed
/// sequence, or the journal buffer's arena, from position `start` on.
struct Piece {
    bool arena = false;
    std::uint64_t start = 0;
    std::uint64_t len = 0;

    std::uint64_t length() const { return len; }
    /// Whether `next` continues this piece's bytes where they end.
    bool continued_by(Piece const& next) const {
        return arena == next.arena && start + len == next.start;
    }
    bool operator==(Piece const&) const = default;
};

/// Applies the records `[from, to)` of `records` to a sequence of `committed`
/// bytes, and returns the new sequence as pieces, adjacent pieces that
/// continue each other merged. `first` is the number of the journal's first
/// record: a paste of a cut that a record before it made is applied as the
/// splice of its fallback bytes, as the journal holds it.
///
/// Fails on a record whose range does not lie inside the sequence, or on a
/// paste of a cut its journal did not make or already pasted.
expected<std::vector<Piece>> fold(Records const& records, Cursor from, Cursor to,
                                  std::uint64_t committed, std::uint64_t first);

}  // namespace kladde::store

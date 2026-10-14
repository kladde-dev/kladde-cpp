// Fingerprints (spec/schema/fingerprints): a white/grey/black depth-first
// search that hashes each node's canonical encoding, with names left out
// and every reference replaced by a token, either the child's fingerprint or,
// for an edge back to a node still being hashed, how many levels up it is.
#include "internal.hpp"

#include <kladde/varint.hpp>

#include <algorithm>

namespace kladde::schema {

namespace {

enum class Color : std::uint8_t { white, grey, black };

// The search keeps an explicit stack rather than recursing, so that a long
// chain of descriptors in a decoded table cannot overflow the call stack.
class Traversal {
public:
    explicit Traversal(TypeTable const& table)
        : table_(table),
          color_(table.descriptors().size(), Color::white),
          depth_(table.descriptors().size(), 0),
          memo_(table.descriptors().size()) {}

    Fingerprint visit(std::size_t root) {
        enter(root, 0);
        Fingerprint done;
        while (!stack_.empty()) {
            auto& top = stack_.back();
            if (top.next < top.layout.slots.size()) {
                auto child = top.layout.slots[top.next].ref.index;
                switch (color_.at(child)) {
                    case Color::white:
                        // Its token follows once it is hashed, below.
                        enter(child, top.depth + 1);
                        continue;
                    case Color::black:
                        token(top, memo_[child]);
                        break;
                    case Color::grey: {
                        std::vector<std::uint8_t> back{1};
                        put_varint(back, top.depth - depth_[child]);
                        top.hasher.update(back);
                        advance(top);
                        break;
                    }
                }
                continue;
            }
            auto digest = top.hasher.finalize();
            Fingerprint fp;
            std::copy_n(digest.begin(), fp.bytes.size(), fp.bytes.begin());
            memo_[top.node] = fp;
            color_[top.node] = Color::black;
            stack_.pop_back();
            if (stack_.empty()) {
                done = fp;
            } else {
                token(stack_.back(), fp);
            }
        }
        return done;
    }

private:
    struct Frame {
        std::size_t node;
        std::size_t depth;
        detail::Layout layout;
        std::size_t next = 0;  // the next slot to emit a token for
        Sha256 hasher;
    };

    void enter(std::size_t node, std::size_t depth) {
        color_.at(node) = Color::grey;
        depth_[node] = depth;
        Frame f{node, depth, detail::layout(table_.descriptors()[node], detail::LayoutFor::hashing), 0, Sha256()};
        auto end = f.layout.slots.empty() ? f.layout.bytes.size() : f.layout.slots[0].at;
        f.hasher.update(std::span(f.layout.bytes).first(end));
        stack_.push_back(std::move(f));
    }

    // Emits a child's fingerprint as the token for the frame's current
    // slot, then the bytes up to its next slot.
    static void token(Frame& f, Fingerprint const& child) {
        f.hasher.update(std::uint8_t{0});
        f.hasher.update(child.bytes);
        advance(f);
    }

    static void advance(Frame& f) {
        auto const& slots = f.layout.slots;
        auto from = slots[f.next].at;
        ++f.next;
        auto to = f.next < slots.size() ? slots[f.next].at : f.layout.bytes.size();
        f.hasher.update(std::span(f.layout.bytes).subspan(from, to - from));
    }

    TypeTable const& table_;
    std::vector<Color> color_;
    std::vector<std::size_t> depth_;
    std::vector<Fingerprint> memo_;
    std::vector<Frame> stack_;
};

}  // namespace

Fingerprint TypeTable::fingerprint() const { return fingerprint(root()); }

Fingerprint TypeTable::fingerprint(TypeRef r) const { return Traversal(*this).visit(r.index); }

}  // namespace kladde::schema

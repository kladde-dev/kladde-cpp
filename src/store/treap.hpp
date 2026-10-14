// An implicit treap: a sequence of items, each with a length, kept in a
// randomized balanced binary tree whose nodes know the total length below
// them. The item at a position is found, a sequence is cut at a position, and
// two sequences are joined, each in O(log n) expected steps.
//
// One arena holds any number of trees, each named by its root node; the fold
// keeps the cuts of a transaction as trees of their own beside the piece
// table, and the store keeps the rope over the sequence's spans. Nodes know
// their parent, so an item's position, and its neighbours, are found from its
// node.
#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "util.hpp"

namespace kladde::store {

/// The empty tree, and the absence of a node.
inline constexpr std::uint32_t NIL = 0;

/// An arena of treap nodes, holding any number of trees. `T` has a
/// `std::uint64_t length() const`.
template <class T>
class Treap {
public:
    Treap() { nodes_.push_back(Node{}); }

    /// A tree of one node holding `item`.
    std::uint32_t node(T item) {
        Node n{item, NIL, NIL, NIL, prio(), item.length()};
        if (!free_.empty()) {
            std::uint32_t i = free_.back();
            free_.pop_back();
            nodes_[i] = n;
            return i;
        }
        nodes_.push_back(n);
        return static_cast<std::uint32_t>(nodes_.size() - 1);
    }

    /// Returns every node of the tree at `root` to the arena.
    void free_tree(std::uint32_t root) {
        std::vector<std::uint32_t> stack{root};
        while (!stack.empty()) {
            std::uint32_t n = stack.back();
            stack.pop_back();
            if (n == NIL) continue;
            stack.push_back(nodes_[n].left);
            stack.push_back(nodes_[n].right);
            free_.push_back(n);
        }
    }

    /// Nodes in use.
    std::size_t live_nodes() const { return nodes_.size() - 1 - free_.size(); }

    T const& item(std::uint32_t n) const { return nodes_[n].item; }

    /// The total length of the tree at `root`.
    std::uint64_t sum(std::uint32_t root) const { return root == NIL ? 0 : nodes_[root].sum; }

    /// Replaces the item of node `n`, whose length may change, and corrects
    /// the sums of its ancestors.
    void set_item(std::uint32_t n, T item) {
        nodes_[n].item = item;
        for (std::uint32_t m = n; m != NIL; m = nodes_[m].parent)
            nodes_[m].sum = sum(nodes_[m].left) + sum(nodes_[m].right) + nodes_[m].item.length();
    }

    /// The root of the tree that holds node `n`.
    std::uint32_t root_of(std::uint32_t n) const {
        std::uint32_t m = n;
        while (nodes_[m].parent != NIL) m = nodes_[m].parent;
        return m;
    }

    /// Joins the trees at `a` and `b`, `a`'s items first.
    std::uint32_t merge(std::uint32_t a, std::uint32_t b) {
        if (a == NIL) {
            if (b != NIL) nodes_[b].parent = NIL;
            return b;
        }
        if (b == NIL) {
            nodes_[a].parent = NIL;
            return a;
        }
        if (nodes_[a].prio > nodes_[b].prio) {
            std::uint32_t m = merge(nodes_[a].right, b);
            nodes_[a].right = m;
            pull(a);
            nodes_[a].parent = NIL;
            return a;
        }
        std::uint32_t m = merge(a, nodes_[b].left);
        nodes_[b].left = m;
        pull(b);
        nodes_[b].parent = NIL;
        return b;
    }

    /// Splits the tree at `root` into the items before position `pos` and
    /// those from it on. `pos` must fall on a boundary between items.
    std::pair<std::uint32_t, std::uint32_t> split(std::uint32_t root, std::uint64_t pos) {
        if (root == NIL) return {NIL, NIL};
        std::uint64_t ls = sum(nodes_[root].left);
        std::uint64_t own = nodes_[root].item.length();
        if (pos <= ls) {
            auto [a, b] = split(nodes_[root].left, pos);
            nodes_[root].left = b;
            pull(root);
            nodes_[root].parent = NIL;
            if (a != NIL) nodes_[a].parent = NIL;
            return {a, root};
        }
        KLADDE_DEBUG_CHECK(pos >= ls + own, "splitting inside an item");
        auto [a, b] = split(nodes_[root].right, pos - ls - own);
        nodes_[root].right = a;
        pull(root);
        nodes_[root].parent = NIL;
        if (b != NIL) nodes_[b].parent = NIL;
        return {root, b};
    }

    /// The node whose item holds position `pos` of the tree at `root`, and
    /// how far into the item `pos` lies; nothing past the end.
    std::optional<std::pair<std::uint32_t, std::uint64_t>> find(std::uint32_t root,
                                                                 std::uint64_t pos) const {
        std::uint32_t n = root;
        while (n != NIL) {
            Node const& node = nodes_[n];
            std::uint64_t ls = sum(node.left);
            if (pos < ls) {
                n = node.left;
            } else if (pos < ls + node.item.length()) {
                return std::pair{n, pos - ls};
            } else {
                pos -= ls + node.item.length();
                n = node.right;
            }
        }
        return std::nullopt;
    }

    /// Where the item of node `n` starts within its tree.
    std::uint64_t pos_of(std::uint32_t n) const {
        std::uint64_t pos = sum(nodes_[n].left);
        std::uint32_t m = n;
        for (;;) {
            std::uint32_t p = nodes_[m].parent;
            if (p == NIL) return pos;
            if (nodes_[p].right == m) pos += sum(nodes_[p].left) + nodes_[p].item.length();
            m = p;
        }
    }

    /// The first node of the tree at `root`, in order.
    std::uint32_t first(std::uint32_t root) const {
        std::uint32_t n = root;
        if (n == NIL) return NIL;
        while (nodes_[n].left != NIL) n = nodes_[n].left;
        return n;
    }

    /// The last node of the tree at `root`, in order.
    std::uint32_t last(std::uint32_t root) const {
        std::uint32_t n = root;
        if (n == NIL) return NIL;
        while (nodes_[n].right != NIL) n = nodes_[n].right;
        return n;
    }

    /// The node after `n`, in order, or `NIL`.
    std::uint32_t next(std::uint32_t n) const {
        if (nodes_[n].right != NIL) return first(nodes_[n].right);
        std::uint32_t m = n;
        for (;;) {
            std::uint32_t p = nodes_[m].parent;
            if (p == NIL) return NIL;
            if (nodes_[p].left == m) return p;
            m = p;
        }
    }

    /// The node before `n`, in order, or `NIL`.
    std::uint32_t prev(std::uint32_t n) const {
        if (nodes_[n].left != NIL) return last(nodes_[n].left);
        std::uint32_t m = n;
        for (;;) {
            std::uint32_t p = nodes_[m].parent;
            if (p == NIL) return NIL;
            if (nodes_[p].right == m) return p;
            m = p;
        }
    }

    /// Splits the tree at `root` at position `pos`, cutting the item that
    /// holds it with `cut(item, off)`, which returns the item's two parts.
    template <class Cut>
    std::pair<std::uint32_t, std::uint32_t> split_at(std::uint32_t root, std::uint64_t pos,
                                                     Cut&& cut) {
        auto found = find(root, pos);
        if (found && found->second > 0) {
            auto [n, off] = *found;
            auto [a, b] = cut(nodes_[n].item, off);
            set_item(n, a);
            std::uint32_t r = root_of(n);
            auto [l, rest] = split(r, pos);
            std::uint32_t m = node(b);
            return {l, merge(m, rest)};
        }
        return split(root, pos);
    }

    /// The nodes of the tree at `root`, in order.
    std::vector<std::uint32_t> nodes(std::uint32_t root) const {
        std::vector<std::uint32_t> out;
        std::vector<std::uint32_t> stack;
        std::uint32_t n = root;
        while (n != NIL || !stack.empty()) {
            while (n != NIL) {
                stack.push_back(n);
                n = nodes_[n].left;
            }
            std::uint32_t m = stack.back();
            stack.pop_back();
            out.push_back(m);
            n = nodes_[m].right;
        }
        return out;
    }

    /// Checks the sums and parent links of the tree at `root`, aborting at
    /// the first violation.
    void check(std::uint32_t root) const {
        if (root == NIL) return;
        KLADDE_CHECK(nodes_[root].parent == NIL, "a root has a parent");
        std::vector<std::uint32_t> stack{root};
        while (!stack.empty()) {
            std::uint32_t n = stack.back();
            stack.pop_back();
            Node const& node = nodes_[n];
            KLADDE_CHECK(node.sum == sum(node.left) + sum(node.right) + node.item.length(),
                         "a treap node has a wrong sum");
            for (std::uint32_t c : {node.left, node.right}) {
                if (c == NIL) continue;
                KLADDE_CHECK(nodes_[c].parent == n, "a treap node has a wrong parent");
                KLADDE_CHECK(nodes_[c].prio <= node.prio, "a treap violates heap order");
                stack.push_back(c);
            }
        }
    }

private:
    struct Node {
        T item{};
        std::uint32_t left = NIL;
        std::uint32_t right = NIL;
        std::uint32_t parent = NIL;
        std::uint32_t prio = 0;
        /// The total length of the subtree.
        std::uint64_t sum = 0;
    };

    std::uint32_t prio() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 7;
        rng_ ^= rng_ << 17;
        return static_cast<std::uint32_t>(rng_ >> 32);
    }

    void pull(std::uint32_t n) {
        std::uint32_t l = nodes_[n].left, r = nodes_[n].right;
        nodes_[n].sum = sum(l) + sum(r) + nodes_[n].item.length();
        if (l != NIL) nodes_[l].parent = n;
        if (r != NIL) nodes_[r].parent = n;
    }

    std::vector<Node> nodes_;
    std::vector<std::uint32_t> free_;
    std::uint64_t rng_ = 0x9E3779B97F4A7C15;
};

}  // namespace kladde::store

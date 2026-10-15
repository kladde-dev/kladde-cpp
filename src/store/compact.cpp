// Closing into a fully consolidated file
// (impl/flush.md#closing-into-a-consolidated-file): the journal folded in
// memory, the schema, the sequence and the private chain written back to back
// into full data pages of a new file, one `Ref` for each run of a chain within
// a page, and the new file renamed over the old one.
//
// Nothing is flushed into the old file, and the new file's bytes pass through
// one page buffer: the sequence is never held whole.
#include <array>
#include <cstring>
#include <string>

#include "inner.hpp"
#include "util.hpp"

namespace kladde {

using namespace store;

namespace {

/// Fills consecutive data pages from page 2 on, and records the spans the
/// bytes of the chain being written take: address and length.
class Packer {
public:
    Packer(Storage& out, std::uint64_t epoch) : out_(out), epoch_(epoch), buf_(new_page()) {}

    expected<void> push(std::span<std::uint8_t const> bytes) {
        while (!bytes.empty()) {
            std::size_t n = std::min(bytes.size(), MAX_PAGE_CONTENT - used_);
            std::size_t at = CONTENT_OFFSET + used_;
            std::memcpy(buf_->data() + at, bytes.data(), n);
            std::uint64_t addr = std::uint64_t{page_} * PAGE_SIZE + at;
            if (!spans_.empty() && spans_.back().first + spans_.back().second == addr) {
                spans_.back().second += static_cast<std::uint32_t>(n);
            } else {
                spans_.emplace_back(addr, static_cast<std::uint32_t>(n));
            }
            used_ += n;
            bytes = bytes.subspan(n);
            if (used_ == MAX_PAGE_CONTENT) KLADDE_TRY(write_page());
        }
        return {};
    }

    expected<void> push_zeros(std::uint64_t n) {
        static std::array<std::uint8_t, MAX_PAGE_CONTENT> const zeros{};
        while (n > 0) {
            std::size_t k = std::min<std::uint64_t>(n, zeros.size());
            KLADDE_TRY(push(std::span(zeros.data(), k)));
            n -= k;
        }
        return {};
    }

    /// The spans of the chain written since the last call.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> end_chain() { return std::exchange(spans_, {}); }

    /// Writes the last page, if it is partly filled; returns the page after
    /// the data pages.
    expected<std::uint32_t> finish() {
        if (used_ > 0) KLADDE_TRY(write_page());
        return page_;
    }

private:
    expected<void> write_page() {
        seal_page(*buf_, nullptr, KIND_DATA, epoch_, used_);
        KLADDE_TRY(out_.write_at(*buf_, std::uint64_t{page_} * PAGE_SIZE));
        ++page_;
        used_ = 0;
        return {};
    }

    Storage& out_;
    std::uint64_t epoch_;
    std::unique_ptr<PageBuf> buf_;
    std::uint32_t page_ = 2;
    std::size_t used_ = 0;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> spans_;
};

/// The last data page read from the committed file, kept for the spans after
/// it that show bytes of the same page.
struct PageCache {
    std::uint32_t page = 0;
    std::unique_ptr<PageBuf> buf;
};

/// A data page of the committed file: from the mirror, or read into `cache`.
expected<PageBuf const*> committed_page(Store::Inner const& in, std::uint32_t page, PageCache& cache) {
    if (auto it = in.mirror.find(page); it != in.mirror.end()) return it->second.get();
    if (!cache.buf || cache.page != page) {
        if (!cache.buf) cache.buf = new_page();
        KLADDE_TRY(in.storage->read_at(*cache.buf, std::uint64_t{page} * PAGE_SIZE));
        if (!std::holds_alternative<DecodedPage>(decode_page(*cache.buf, false))) {
            cache.buf.reset();
            return corrupt("data page " + std::to_string(page) + " fails its checksum");
        }
        cache.page = page;
    }
    return cache.buf.get();
}

/// Writes `len` bytes of the committed sequence, from position `pos` on.
expected<void> copy_committed(Store::Inner const& in, std::uint64_t pos, std::uint64_t len, Packer& p,
                              PageCache& cache) {
    auto found = in.state.rope.find(in.state.root, pos);
    if (!found) return corrupt("a piece reaches past the committed sequence");
    auto [node, off] = *found;
    for (std::uint64_t left = len; left > 0;) {
        if (node == NIL) return corrupt("a piece reaches past the committed sequence");
        RopeItem item = in.state.rope.item(node);
        KLADDE_CHECK(!item.lit, "a literal in the rope between flushes");
        std::uint64_t take = std::min<std::uint64_t>(item.len - off, left);
        Content c = in.state.span(item.id_or_start).content;
        switch (c.kind) {
            case Content::Kind::ref: {
                KLADDE_TRY_ASSIGN(PageBuf const* buf, committed_page(in, c.page, cache));
                KLADDE_TRY(p.push(std::span(buf->data() + c.offset + off, take)));
                break;
            }
            case Content::Kind::inline_:
                KLADDE_TRY(p.push(std::span(in.table_pages.at(c.page)->data() + c.offset + off, take)));
                break;
            case Content::Kind::zero: KLADDE_TRY(p.push_zeros(take)); break;
            default: KLADDE_CHECK(false, "a span shows a pending or free content between flushes");
        }
        left -= take;
        off = 0;
        node = in.state.rope.next(node);
    }
    return {};
}

}  // namespace

expected<void> Store::Inner::close_consolidated() {
    if (poisoned) return errc::poisoned;
    KLADDE_TRY(join_pending());
    KLADDE_TRY_ASSIGN(auto target, storage->sibling());
    // The old file keeps everything until the new one replaces it.
    KLADDE_TRY_ASSIGN(Cursor committed, append_ready());
    KLADDE_TRY_ASSIGN(auto pieces, fold(ops, ops.front(), committed, state.seq_len(), ops.front().at));
    auto const& schema_bytes = schema_pending ? *schema_pending : schema;
    auto const& private_bytes = private_pending ? *private_pending : private_;
    std::uint64_t seq_len = 0;
    for (auto const& piece : pieces) seq_len += piece.len;
    std::uint64_t total = schema_bytes.size() + seq_len + private_bytes.size();
    std::uint64_t data_pages = (total + MAX_PAGE_CONTENT - 1) / MAX_PAGE_CONTENT;
    if (2 + data_pages >= UINT32_MAX) return errc::out_of_bounds;
    // Even, so that the header goes to slot 0, and page 1 is left blank.
    std::uint64_t e = (epoch + 2) & ~std::uint64_t{1};
    KLADDE_TRY(target->resize((2 + data_pages) * PAGE_SIZE));

    // 1. The data pages.
    Packer p(*target, e);
    KLADDE_TRY(p.push(schema_bytes));
    auto schema_spans = p.end_chain();
    PageCache last_read;
    for (auto const& piece : pieces) {
        if (piece.arena) {
            KLADDE_TRY(p.push(ops.slice(piece.start, piece.len)));
        } else {
            KLADDE_TRY(copy_committed(*this, piece.start, piece.len, p, last_read));
        }
    }
    auto seq_spans = p.end_chain();
    KLADDE_TRY(p.push(private_bytes));
    auto private_spans = p.end_chain();
    KLADDE_TRY_ASSIGN(std::uint32_t leaf0, p.finish());

    // 2. The span table: ids in chain order, each linking to the next.
    std::vector<EntryOut> entries;
    std::array<std::uint32_t, 3> heads{};
    std::array<std::vector<std::pair<std::uint64_t, std::uint32_t>> const*, 3> chains{
        &schema_spans, &seq_spans, &private_spans};
    for (std::size_t c = 0; c < chains.size(); ++c) {
        auto const& spans = *chains[c];
        for (std::size_t i = 0; i < spans.size(); ++i) {
            auto id = static_cast<std::uint32_t>(entries.size() + 1);
            if (i == 0) heads[c] = id;
            std::uint32_t next = i + 1 < spans.size() ? id + 1 : 0;
            entries.push_back({id, next, Body::ref(spans[i].second, spans[i].first)});
        }
    }
    PageEncoder header_enc({}, MAX_HEADER_CONTENT);
    std::vector<std::vector<std::uint8_t>> leaves;
    bool fits = true;
    for (auto const& entry : entries) {
        if (!header_enc.push(entry)) {
            fits = false;
            break;
        }
    }
    if (!fits) {
        PageEncoder leaf({}, MAX_PAGE_CONTENT);
        for (auto const& entry : entries) {
            if (!leaf.push(entry)) {
                leaves.push_back(std::move(leaf.out));
                leaf = PageEncoder({}, MAX_PAGE_CONTENT);
                KLADDE_CHECK(leaf.push(entry), "an entry fits an empty leaf");
            }
        }
        leaves.push_back(std::move(leaf.out));
        std::vector<std::uint32_t> children;
        for (std::size_t i = 0; i < leaves.size(); ++i) children.push_back(leaf0 + static_cast<std::uint32_t>(i));
        if (children_len(children) > MAX_HEADER_CONTENT) return errc::out_of_bounds;
        header_enc = PageEncoder(children, MAX_HEADER_CONTENT);
    }
    auto pages = static_cast<std::uint32_t>(leaf0 + leaves.size());
    auto buf = new_page();
    if (!leaves.empty()) KLADDE_TRY(target->resize(std::uint64_t{pages} * PAGE_SIZE));
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        encode_page(*buf, nullptr, KIND_TABLE, e, leaves[i]);
        KLADDE_TRY(target->write_at(*buf, (std::uint64_t{leaf0} + i) * PAGE_SIZE));
    }
    HeaderFields fields;
    fields.format_version = FORMAT_VERSION;
    fields.min_reader_version = MIN_READER_VERSION;
    fields.schema_head = heads[0];
    fields.sequence_head = heads[1];
    fields.private_head = heads[2];
    fields.root_fingerprint = header.root_fingerprint;
    // Past the file: the journal is empty.
    fields.journal_pointer = pages;
    encode_page(*buf, &fields, KIND_TABLE, e, header_enc.out);
    KLADDE_TRY(target->write_at(*buf, 0));

    // 3. Durable, then in the old file's place.
    KLADDE_TRY(target->sync());
    return storage->replace_with(std::move(target));
}

expected<void> Store::close_consolidated() {
    if (inner_->poisoned) return errc::poisoned;
    auto r = inner_->close_consolidated();
    // What is in memory describes the old file, or a file half replaced.
    inner_->poisoned = true;
    return r;
}

}  // namespace kladde

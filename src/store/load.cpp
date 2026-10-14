#include "load.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>

#include "entries.hpp"
#include "util.hpp"

namespace kladde::store {

expected<PickedHeader> pick_header(Storage const& storage) {
    KLADDE_TRY_ASSIGN(std::uint64_t len, storage.size());
    if (len < PAGE_SIZE) return errc::not_kladde;
    std::optional<PickedHeader> best;
    bool saw_magic = false;
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        if ((std::uint64_t{slot} + 1) * PAGE_SIZE > len) break;
        auto page = new_page();
        KLADDE_TRY(storage.read_at(*page, std::uint64_t{slot} * PAGE_SIZE));
        if (std::memcmp(page->data(), MAGIC.data(), MAGIC.size()) == 0) saw_magic = true;
        auto decoded = decode_page(*page, true);
        auto* d = std::get_if<DecodedPage>(&decoded);
        if (!d || d->kind != KIND_TABLE || d->epoch % 2 != slot) continue;
        if (!best || d->epoch > best->epoch)
            best = PickedHeader{slot, d->epoch, *d->header, std::move(page)};
    }
    if (!best) {
        if (saw_magic) return corrupt("neither header slot is valid");
        return errc::not_kladde;
    }
    if (best->fields.min_reader_version > READER_VERSION)
        return error{errc::unsupported_version,
                     "it needs reader version " + std::to_string(best->fields.min_reader_version) +
                         " or newer"};
    return std::move(*best);
}

namespace {

/// A span id's newest entry so far.
struct Newest {
    std::uint64_t epoch = 0;
    std::uint32_t page = 0;
    EntryIn entry;
    /// Where the page's content starts within the page.
    std::size_t content_at = 0;
};

/// The epoch in a leaf's framing.
std::uint64_t table_epoch(PageBuf const& buf) { return load_u64(buf.data() + 1); }

}  // namespace

expected<Loaded> load(Storage const& storage, PickedHeader header) {
    KLADDE_TRY_ASSIGN(std::uint64_t file_len, storage.size());
    auto file_pages = static_cast<std::uint32_t>(std::min<std::uint64_t>(file_len / PAGE_SIZE, UINT32_MAX));
    Loaded out;
    State& state = out.state;
    state.flush_epoch = header.epoch;
    state.ensure_page(std::max<std::uint32_t>(file_pages > 0 ? file_pages - 1 : 0, 1));
    IdMap<Newest> newest;
    IdSet seen_page;

    // The header and the leaves it names.
    auto hd = decode_page(*header.page, true);
    auto* hdp = std::get_if<DecodedPage>(&hd);
    if (!hdp) return corrupt("the header fails its checks");
    KLADDE_TRY_ASSIGN(Decoded decoded,
                      decode_entries(std::span(header.page->data() + hdp->content_start,
                                               hdp->content_end - hdp->content_start)));
    std::vector<std::uint32_t> leaves = decoded.children;
    std::vector<std::tuple<std::uint32_t, std::uint64_t, Decoded, std::size_t>> pages_to_read;
    pages_to_read.emplace_back(header.slot, header.epoch, std::move(decoded), HEADER_CONTENT_OFFSET);
    for (std::uint32_t leaf : leaves) {
        if (leaf < 2 || leaf >= file_pages || !seen_page.insert(leaf).second)
            return corrupt("the header names leaf " + std::to_string(leaf) + " out of bounds or twice");
        auto buf = new_page();
        KLADDE_TRY(storage.read_at(*buf, std::uint64_t{leaf} * PAGE_SIZE));
        auto dl = decode_page(*buf, false);
        auto* d = std::get_if<DecodedPage>(&dl);
        if (!d) return corrupt("leaf " + std::to_string(leaf) + " fails its checks");
        if (d->kind != KIND_TABLE || d->epoch > header.epoch)
            return corrupt("leaf " + std::to_string(leaf) + " is not a table page of the header's world");
        KLADDE_TRY_ASSIGN(Decoded dec, decode_entries(std::span(buf->data() + d->content_start,
                                                                d->content_end - d->content_start)));
        if (!dec.children.empty())
            return corrupt("leaf " + std::to_string(leaf) +
                           " names pages of its own, which this implementation does not read");
        pages_to_read.emplace_back(leaf, d->epoch, std::move(dec), CONTENT_OFFSET);
        out.table_pages[leaf] = std::move(buf);
    }
    out.table_pages[header.slot] = std::make_unique<PageBuf>(*header.page);
    for (auto& [page, epoch, dec, content_at] : pages_to_read) {
        for (EntryIn const& e : dec.entries) {
            auto it = newest.find(e.id);
            if (it != newest.end() && it->second.epoch == epoch)
                return corrupt("span " + std::to_string(e.id) + " has two entries of epoch " +
                               std::to_string(epoch));
            if (it != newest.end() && it->second.epoch > epoch) continue;
            newest[e.id] = Newest{epoch, page, e, content_at};
        }
    }

    // The chains.
    IdSet reached;
    auto walk = [&](std::uint32_t head, Chain chain) -> expected<std::vector<std::uint32_t>> {
        std::vector<std::uint32_t> list;
        std::uint32_t id = head;
        while (id != 0) {
            if (!reached.insert(id).second)
                return corrupt("span " + std::to_string(id) + " is reached twice");
            auto it = newest.find(id);
            if (it == newest.end())
                return corrupt("the chain names span " + std::to_string(id) +
                               ", which no entry describes");
            Newest const& n = it->second;
            std::uint32_t len = n.entry.len;
            Content content;
            switch (n.entry.kind) {
                case Body::Kind::ref: {
                    auto page = static_cast<std::uint32_t>(n.entry.addr / PAGE_SIZE);
                    auto offset = static_cast<std::uint16_t>(n.entry.addr % PAGE_SIZE);
                    if (page >= file_pages)
                        return corrupt("span " + std::to_string(id) + " shows a page past the file");
                    content = Content::ref(page, offset);
                    break;
                }
                case Body::Kind::zero: content = Content::zero(); break;
                case Body::Kind::inline_:
                    content = Content::inline_at(n.page,
                                                 static_cast<std::uint16_t>(n.content_at + n.entry.offset));
                    break;
            }
            if (state.spans.size() <= id) state.spans.resize(std::size_t{id} + 1);
            state.ids.mark(id);
            Span& s = state.spans[id];
            s = Span{};
            s.len = len;
            s.next = n.entry.next;
            s.content = content;
            s.chain = chain;
            s.entry_page = NONE;
            s.node = NIL;
            s.heat = n.epoch;
            list.push_back(id);
            id = n.entry.next;
        }
        return list;
    };
    KLADDE_TRY_ASSIGN(auto seq, walk(header.fields.sequence_head, Chain::sequence));
    KLADDE_TRY_ASSIGN(auto schema, walk(header.fields.schema_head, Chain::schema));
    KLADDE_TRY_ASSIGN(auto priv, walk(header.fields.private_head, Chain::private_));
    std::vector<std::uint32_t> all = seq;
    all.insert(all.end(), schema.begin(), schema.end());
    all.insert(all.end(), priv.begin(), priv.end());

    // The pages: the header, the leaves, and the data pages the chains show.
    state.pages[header.slot] = Page::live(header.epoch, 0, PageKind::header);
    for (std::uint32_t leaf : leaves) {
        state.pages[leaf] = Page::live(table_epoch(*out.table_pages.at(leaf)), 0, PageKind::table);
        // A leaf that keeps no live entry is unlinked by the next flush.
        state.dropped_candidates.insert(leaf);
    }
    std::vector<std::uint32_t> data_pages;
    for (std::uint32_t id : all) {
        Content c = state.spans[id].content;
        if (c.kind != Content::Kind::ref) continue;
        if (!state.pages[c.page].is_live()) {
            data_pages.push_back(c.page);
            state.pages[c.page] = Page::live(0, 0, PageKind::data);
        } else if (state.pages[c.page].kind != PageKind::data) {
            return corrupt("span " + std::to_string(id) + " shows a table page's bytes");
        }
    }
    std::sort(data_pages.begin(), data_pages.end());
    // Where each data page's content ends: a span must show bytes inside it
    // (spec/span-table.md#resolution), not the padding after it.
    IdMap<std::size_t> content_end;
    for (std::uint32_t p : data_pages) {
        auto buf = new_page();
        KLADDE_TRY(storage.read_at(*buf, std::uint64_t{p} * PAGE_SIZE));
        auto dd = decode_page(*buf, false);
        auto* d = std::get_if<DecodedPage>(&dd);
        if (!d) return corrupt("data page " + std::to_string(p) + " fails its checks");
        if (d->kind != KIND_DATA || d->epoch > header.epoch)
            return corrupt("page " + std::to_string(p) + " is not a data page of the header's world");
        state.live_mut(p).epoch = d->epoch;
        content_end[p] = d->content_end;
        out.mirror[p] = std::move(buf);
    }
    // Coverage, the reverse lists, and the rope.
    for (std::uint32_t id : all) {
        Newest const& n = newest.at(id);
        Span s = state.spans[id];
        if (s.content.kind == Content::Kind::ref &&
            std::size_t{s.content.offset} + s.len > content_end.at(s.content.page))
            return corrupt("span " + std::to_string(id) + " reaches past its page's content");
        state.note_data(id);
        if (s.content.kind == Content::Kind::ref) state.cover(s.content.page, s.len);
        state.note_entry(id, n.page, n.entry.entry_len);
    }
    std::uint32_t root = NIL;
    for (std::uint32_t id : seq) {
        std::uint32_t node = state.rope.node(RopeItem::span(id, state.spans[id].len));
        state.spans[id].node = node;
        root = state.rope.merge(root, node);
    }
    state.root = root;
    state.schema = std::move(schema);
    state.private_ = std::move(priv);
    for (std::uint32_t p = 0; p < state.pages.size(); ++p)
        if (state.pages[p].is_live()) state.rebucket(p);
    auto read_chain = [&](std::vector<std::uint32_t> const& list) {
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t id : list) {
            Span const& s = state.span(id);
            std::uint8_t const* from = nullptr;
            if (s.content.kind == Content::Kind::ref)
                from = out.mirror.at(s.content.page)->data() + s.content.offset;
            else if (s.content.kind == Content::Kind::inline_)
                from = out.table_pages.at(s.content.page)->data() + s.content.offset;
            if (from)
                bytes.insert(bytes.end(), from, from + s.len);
            else
                bytes.resize(bytes.size() + s.len, 0);
        }
        return bytes;
    };
    out.schema = read_chain(state.schema);
    out.private_ = read_chain(state.private_);
    out.file_pages = file_pages;
    out.header = std::move(header);
    return out;
}

}  // namespace kladde::store

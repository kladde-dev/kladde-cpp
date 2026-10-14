// Opening a file: pick the header, read every span-table page it reaches and
// keep each span id's newest entry, walk the chains from their heads, and
// read the data pages they show, in page order.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <kladde/store.hpp>

#include "page.hpp"
#include "state.hpp"

namespace kladde::store {

/// The governing header.
struct PickedHeader {
    std::uint32_t slot = 0;
    std::uint64_t epoch = 0;
    HeaderFields fields;
    std::unique_ptr<PageBuf> page;
};

/// Reads both header slots and picks the CRC-valid one with the higher epoch.
expected<PickedHeader> pick_header(Storage const& storage);

/// Everything a load produces.
struct Loaded {
    State state;
    PickedHeader header;
    /// Resident span-table pages, the governing header's included.
    IdMap<std::unique_ptr<PageBuf>> table_pages;
    /// Data pages the chains show.
    IdMap<std::unique_ptr<PageBuf>> mirror;
    /// The number of pages in the file.
    std::uint32_t file_pages = 0;
    /// The content of the schema's chain and of the private chain.
    std::vector<std::uint8_t> schema;
    std::vector<std::uint8_t> private_;
};

/// Loads the world of `header`.
expected<Loaded> load(Storage const& storage, PickedHeader header);

}  // namespace kladde::store

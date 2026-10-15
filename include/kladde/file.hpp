// `kladde::File<T>`: a root value paired with the file that backs it.
#pragma once

#include <kladde/typed/list.hpp>
#include <kladde/typed/map.hpp>

namespace kladde {

/// A root durable value paired with the file that backs it: kladde-rs's
/// `Kladde<T>`.
///
/// Reading is plain C++: `get()` hands out `T const&`. Mutating goes through
/// `edit()`, whose handle records every change in the file's journal before
/// it changes memory, so there is no save call. The journal is folded into
/// the file by flushes, which happen by themselves as it fills, and on
/// `flush`, `flush_if_journal_exceeds` and `close`.
///
/// Opening and creating return an `expected`, since their failures are
/// ordinary. A mutation or a flush fails only when the disk does; the file is
/// then poisoned, and the failure is thrown as `kladde::exception` (or, in a
/// build without exceptions, passed to the handler `set_error_handler`
/// installs), and every later mutation fails the same way.
///
/// ```cpp
/// struct Settings { std::uint8_t volume; Theme theme; };
/// KLADDE_STRUCT(Settings, volume, theme)
///
/// auto file = kladde::File<Settings>::create("settings.kladde", Settings{3, Theme::light}).value();
/// file.edit().theme().set(Theme::dark);
/// file.close();
/// auto again = kladde::File<Settings>::open("settings.kladde").value();
/// assert(again.get().theme == Theme::dark);
/// ```
template <Durable T>
class File {
public:
    /// Creates the file at `path`, replacing any file there, holding `root`.
    static expected<File> create(std::filesystem::path const& path, T root, Options opts = {}) {
        auto storage = FileStorage::create(path);
        if (!storage) return std::move(storage).error();
        return create_in(std::move(*storage), std::move(root), opts);
    }
    /// Creates a kladde file holding `root` in `storage`.
    static expected<File> create_in(std::unique_ptr<Storage> storage, T root, Options opts = {}) {
        auto store = Store::create(std::move(storage), opts);
        if (!store) return std::move(store).error();
        auto table = schema_of<T>();
        store->set_schema(table.encode());
        store->set_root_fingerprint(table.fingerprint().bytes);
        auto bytes = to_bytes(root);
        if (auto r = store->splice(0, 0, bytes); !r) return std::move(r).error();
        if (auto r = store->flush(); !r) return std::move(r).error();
        return File(std::make_unique<State>(std::move(*store), std::move(root)));
    }
    /// Opens the file at `path` and loads its root value, recovering every
    /// mutation that returned before the process that wrote it died. Fails
    /// with `errc::schema_mismatch` if the file's root type is not `T`.
    static expected<File> open(std::filesystem::path const& path, Options opts = {}) {
        auto storage = FileStorage::open(path);
        if (!storage) return std::move(storage).error();
        return open_in(std::move(*storage), opts);
    }
    static expected<File> open_in(std::unique_ptr<Storage> storage, Options opts = {}) {
        auto store = Store::open(std::move(storage), opts);
        if (!store) return std::move(store).error();
        auto expected_fp = fingerprint_of<T>().bytes;
        auto found = store->root_fingerprint();
        if (found != expected_fp) {
            error e{errc::schema_mismatch};
            e.expected_fingerprint = expected_fp;
            e.found_fingerprint = found;
            return e;
        }
        auto bytes = store->read_all();
        if (!bytes) return std::move(bytes).error();
        Decoder in(*bytes);
        T root{};
        if (!durable<T>::decode(in, root) || !in.ok()) return error{errc::corrupt, in.message()};
        if (!in.at_end()) return error{errc::corrupt, "bytes after the root value"};
        store->end_load();
        return File(std::make_unique<State>(std::move(*store), std::move(root)));
    }

    File(File&&) noexcept = default;
    File& operator=(File&&) noexcept = default;

    /// The root value, for reading.
    T const& get() const { return s_->root; }
    T const& operator*() const { return s_->root; }
    T const* operator->() const { return &s_->root; }

    /// The root value's handle, through which every mutation is recorded and
    /// applied. Hold it only as long as it is needed.
    Mut<T> edit() {
        auto len = durable<T>::encoded_len(s_->root);
        assert(len == s_->store.size());
        return Mut<T>(s_->root, Region(s_->store_backend, Link::at_anchor(s_.get(), len), s_.get()));
    }

    /// Runs `f` as one transaction: its mutations reach the journal together,
    /// and a crash keeps all of them or none. `f` takes the root's handle, or
    /// nothing (for edits through `kladde::Ref`s). An exception leaving `f`
    /// poisons the file, since memory already holds what the file will not.
    template <class F>
    void transaction(F&& f) {
        auto r = s_->store_backend.atomically([&]() -> expected<void> {
            if constexpr (std::is_invocable_v<F, Mut<T>>) f(edit());
            else f();
            return {};
        });
        if (!r) report(std::move(r).error());
    }

    /// Runs `f` as one batch: its mutations are appended in as few pieces as
    /// the journal's budget allows. A batch saves journal writes and is no
    /// transaction.
    template <class F>
    void batch(F&& f) {
        begin_batch();
#if KLADDE_EXCEPTIONS
        try {
#endif
            if constexpr (std::is_invocable_v<F, Mut<T>>) f(edit());
            else f();
#if KLADDE_EXCEPTIONS
        } catch (...) {
            (void)s_->store.end_batch();
            throw;
        }
#endif
        end_batch();
    }
    /// Opens a batch that `end_batch` ends, for applications whose batches
    /// do not fit a scope, such as an undo step.
    void begin_batch() { check(s_->store_backend, s_->store.begin_batch()); }
    void end_batch() { check(s_->store_backend, s_->store.end_batch()); }

    /// Folds the journal into the file now.
    void flush() { check(s_->store_backend, s_->store.flush()); }
    /// Flushes if the journal holds at least `fraction` of its budget; returns
    /// whether it flushed. For an interactive application's idle callback:
    /// flushing while the user pauses keeps the flush that a full journal
    /// forces, whose `fsync` blocks the mutation that triggers it, out of the
    /// middle of a gesture. Keep the fraction well above zero, such as 0.5:
    /// a flush ends the journal, and with it every cut still to be pasted.
    bool flush_if_journal_exceeds(double fraction) {
        auto r = s_->store.flush_if_journal_exceeds(fraction);
        if (!check(s_->store_backend, r)) return false;
        return *r;
    }
    /// Flushes, then gives unused space at the end of the file back.
    void close() { check(s_->store_backend, s_->store.close()); }
    /// Closes the file by replacing it with a fully consolidated copy of
    /// itself, the smallest file that holds the value, as
    /// `Store::close_consolidated` describes; for when the user closes a
    /// document. The `File` is poisoned afterwards: open the file again to
    /// go on.
    void close_consolidated() { check(s_->store_backend, s_->store.close_consolidated()); }

    Stats stats() const { return s_->store.stats(); }
    Store& store() { return s_->store; }
    bool is_poisoned() const { return s_->store.is_poisoned(); }

private:
    // The root's anchor, with the store and the value, on the heap so that
    // the lists inside the value may point to it while the `File` moves.
    struct State final : Anchor {
        State(Store st, T r) : store(std::move(st)), store_backend(store), root(std::move(r)) {
            kladde::attach(root, this);
        }
        Store store;
        mutable StoreBackend store_backend;
        T root;

        std::uint64_t start() const override { return 0; }
        void grew(std::int64_t) override {}
        Backend& backend() const override { return store_backend; }
        bool locate(void const* target, std::uint64_t& offset) const override {
            if (static_cast<void const*>(&root) == target) return true;
            return kladde::locate(root, target, offset);
        }
    };

    explicit File(std::unique_ptr<State> s) : s_(std::move(s)) {}
    std::unique_ptr<State> s_;
};

}  // namespace kladde

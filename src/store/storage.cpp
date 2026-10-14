// Where a store's bytes live: a real file, or a byte vector in memory.
//
// The store addresses its storage by byte offset and never relies on a shared
// file cursor, so every operation is positional.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kladde/store.hpp>

namespace kladde {

namespace {

error errno_error(char const* what) {
    return error{errc::io, std::string(what) + ": " + std::strerror(errno)};
}

/// Opens `path` with `flags` and locks it against a second process.
expected<int> open_locked(std::filesystem::path const& path, int flags) {
    int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
    if (fd < 0) return errno_error("open");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        error e = errno == EWOULDBLOCK
                      ? error{errc::io, "the file is locked by another process"}
                      : errno_error("flock");
        ::close(fd);
        return e;
    }
    return fd;
}

}  // namespace

// ---------------------------------------------------------------- FileStorage

expected<std::unique_ptr<FileStorage>> FileStorage::open(std::filesystem::path const& path) {
    KLADDE_TRY_ASSIGN(int fd, open_locked(path, O_RDWR));
    return std::unique_ptr<FileStorage>(new FileStorage(fd));
}

expected<std::unique_ptr<FileStorage>> FileStorage::create(std::filesystem::path const& path) {
    // Truncated only once locked, so that a file another process holds open
    // is never emptied under it.
    KLADDE_TRY_ASSIGN(int fd, open_locked(path, O_RDWR | O_CREAT));
    auto s = std::unique_ptr<FileStorage>(new FileStorage(fd));
    if (::ftruncate(fd, 0) != 0) return errno_error("ftruncate");
    return s;
}

FileStorage::~FileStorage() {
    if (fd_ >= 0) ::close(fd_);
}

expected<std::uint64_t> FileStorage::size() const {
    struct stat st {};
    if (::fstat(fd_, &st) != 0) return errno_error("fstat");
    return static_cast<std::uint64_t>(st.st_size);
}

expected<void> FileStorage::read_at(std::span<std::uint8_t> buf, std::uint64_t offset) const {
    std::size_t done = 0;
    while (done < buf.size()) {
        ssize_t n = ::pread(fd_, buf.data() + done, buf.size() - done,
                            static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_error("pread");
        }
        if (n == 0) return error{errc::io, "pread: unexpected end of file"};
        done += static_cast<std::size_t>(n);
    }
    return {};
}

expected<void> FileStorage::write_at(std::span<std::uint8_t const> buf, std::uint64_t offset) {
    std::size_t done = 0;
    while (done < buf.size()) {
        ssize_t n = ::pwrite(fd_, buf.data() + done, buf.size() - done,
                             static_cast<off_t>(offset + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return errno_error("pwrite");
        }
        done += static_cast<std::size_t>(n);
    }
    return {};
}

expected<void> FileStorage::resize(std::uint64_t len) {
    if (::ftruncate(fd_, static_cast<off_t>(len)) != 0) return errno_error("ftruncate");
    return {};
}

expected<void> FileStorage::sync() {
#ifdef __APPLE__
    // fsync on macOS does not ask the drive to write its cache back.
    if (::fcntl(fd_, F_FULLFSYNC) != 0) return errno_error("fcntl(F_FULLFSYNC)");
#else
    if (::fsync(fd_) != 0) return errno_error("fsync");
#endif
    return {};
}

// ---------------------------------------------------------------- MemoryStorage

struct MemoryStorage::State {
    struct Op {
        /// A write of `bytes` at `offset`, or with `resize`, a resize to
        /// `offset` bytes.
        bool resize = false;
        std::uint64_t offset = 0;
        std::vector<std::uint8_t> bytes;
    };

    std::mutex mutex;
    std::vector<std::uint8_t> current;
    bool track = false;
    std::vector<std::uint8_t> durable;
    std::vector<Op> since_sync;
    bool fail_syncs = false;
    /// Writes left before every operation fails, as if the power went out.
    std::optional<std::uint64_t> writes_left;

    expected<void> dead() {
        if (!writes_left) return {};
        if (*writes_left == 0) return error{errc::io, "injected power cut"};
        --*writes_left;
        return {};
    }
};

MemoryStorage::MemoryStorage() : state_(std::make_shared<State>()) {}

MemoryStorage MemoryStorage::with_crash_tracking() {
    MemoryStorage s;
    s.state_->track = true;
    return s;
}

MemoryStorage MemoryStorage::from_image(std::vector<std::uint8_t> image) {
    MemoryStorage s;
    s.state_->durable = image;
    s.state_->current = std::move(image);
    return s;
}

MemoryStorage MemoryStorage::from_image_with_crash_tracking(std::vector<std::uint8_t> image) {
    MemoryStorage s = from_image(std::move(image));
    s.state_->track = true;
    return s;
}

std::vector<std::uint8_t> MemoryStorage::image() const {
    std::lock_guard lock(state_->mutex);
    return state_->current;
}

std::size_t MemoryStorage::unsynced_writes() const {
    std::lock_guard lock(state_->mutex);
    return state_->since_sync.size();
}

std::vector<std::uint8_t> MemoryStorage::crash_image(
    std::function<bool(std::size_t)> const& keep) const {
    std::lock_guard lock(state_->mutex);
    if (!state_->track) {
        std::fprintf(stderr, "crash_image needs a MemoryStorage with crash tracking\n");
        std::abort();
    }
    std::vector<std::uint8_t> img = state_->durable;
    for (std::size_t i = 0; i < state_->since_sync.size(); ++i) {
        if (!keep(i)) continue;
        auto const& op = state_->since_sync[i];
        if (op.resize) {
            img.resize(op.offset, 0);
        } else {
            std::size_t end = op.offset + op.bytes.size();
            if (img.size() < end) img.resize(end, 0);
            std::memcpy(img.data() + op.offset, op.bytes.data(), op.bytes.size());
        }
    }
    return img;
}

void MemoryStorage::fail_syncs() {
    std::lock_guard lock(state_->mutex);
    state_->fail_syncs = true;
}

void MemoryStorage::fail_after(std::uint64_t n) {
    std::lock_guard lock(state_->mutex);
    state_->writes_left = n;
}

expected<std::uint64_t> MemoryStorage::size() const {
    std::lock_guard lock(state_->mutex);
    return static_cast<std::uint64_t>(state_->current.size());
}

expected<void> MemoryStorage::read_at(std::span<std::uint8_t> buf, std::uint64_t offset) const {
    std::lock_guard lock(state_->mutex);
    if (offset > state_->current.size() || buf.size() > state_->current.size() - offset)
        return error{errc::io, "read past the end of a MemoryStorage"};
    if (!buf.empty()) std::memcpy(buf.data(), state_->current.data() + offset, buf.size());
    return {};
}

expected<void> MemoryStorage::write_at(std::span<std::uint8_t const> buf, std::uint64_t offset) {
    std::lock_guard lock(state_->mutex);
    KLADDE_TRY(state_->dead());
    if (offset > state_->current.size() || buf.size() > state_->current.size() - offset)
        return error{errc::io, "write past the end of a MemoryStorage"};
    if (!buf.empty()) std::memcpy(state_->current.data() + offset, buf.data(), buf.size());
    if (state_->track)
        state_->since_sync.push_back({false, offset, std::vector<std::uint8_t>(buf.begin(), buf.end())});
    return {};
}

expected<void> MemoryStorage::resize(std::uint64_t len) {
    std::lock_guard lock(state_->mutex);
    KLADDE_TRY(state_->dead());
    state_->current.resize(len, 0);
    if (state_->track) state_->since_sync.push_back({true, len, {}});
    return {};
}

expected<void> MemoryStorage::sync() {
    std::lock_guard lock(state_->mutex);
    if (state_->fail_syncs || state_->writes_left == std::uint64_t{0})
        return error{errc::io, "injected fsync failure"};
    if (state_->track) {
        state_->durable = state_->current;
        state_->since_sync.clear();
    }
    return {};
}

}  // namespace kladde

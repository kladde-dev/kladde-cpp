#include <kladde/error.hpp>

#include <cstdio>
#include <mutex>

namespace kladde {

std::string error::what() const {
    std::string prefix;
    switch (code) {
        case errc::io: prefix = "I/O error"; break;
        case errc::corrupt: prefix = "corrupt kladde file"; break;
        case errc::not_kladde: return message.empty() ? "not a kladde file" : "not a kladde file: " + message;
        case errc::unsupported_version: prefix = "the file needs a newer reader"; break;
        case errc::schema_mismatch:
            return "the file's root type differs from the application's (expected " +
                   to_hex(expected_fingerprint) + ", found " + to_hex(found_fingerprint) + ")";
        case errc::poisoned:
            return "the store is poisoned by an earlier failure; reopen the file";
        case errc::out_of_bounds: prefix = "a position or a length is out of bounds"; break;
    }
    return message.empty() ? prefix : prefix + ": " + message;
}

std::string to_hex(std::array<std::uint8_t, 16> const& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (auto b : bytes) {
        s.push_back(digits[b >> 4]);
        s.push_back(digits[b & 15]);
    }
    return s;
}

namespace {
std::mutex handler_mutex;
error_handler& handler() {
    static error_handler h = [](kladde::error const& e) {
        std::fprintf(stderr, "kladde: %s\n", e.what().c_str());
    };
    return h;
}
}  // namespace

error_handler set_error_handler(error_handler h) {
    std::lock_guard lock(handler_mutex);
    return std::exchange(handler(), std::move(h));
}

void report(kladde::error e) {
#if KLADDE_EXCEPTIONS
    throw kladde::exception(std::move(e));
#else
    error_handler h;
    {
        std::lock_guard lock(handler_mutex);
        h = handler();
    }
    if (h) h(e);
#endif
}

}  // namespace kladde

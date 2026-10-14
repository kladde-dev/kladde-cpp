// Errors: what failed, an expected value for calls whose failure the caller
// must handle, and how a failure that ends the session is reported.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define KLADDE_EXCEPTIONS 1
#else
#define KLADDE_EXCEPTIONS 0
#endif

namespace kladde {

/// Why an operation failed.
enum class errc {
    /// Reading or writing the file failed.
    io,
    /// The file violates the format.
    corrupt,
    /// The file is not a kladde file.
    not_kladde,
    /// The file needs a newer reader than this one.
    unsupported_version,
    /// The file's root type differs from the one the application expects.
    schema_mismatch,
    /// An earlier failure made the store's in-memory state untrustworthy:
    /// destroy the `File` and open the file again.
    poisoned,
    /// A position or a length lies outside the sequence or the value that
    /// names it, or the sequence would exceed the format's bounds.
    out_of_bounds,
};

/// A failure: its kind, a message for people, and for a schema mismatch
/// the two fingerprints that differ.
///
/// ```cpp
/// kladde::error e{kladde::errc::corrupt, "a span runs past its page"};
/// assert(e.what() == "corrupt kladde file: a span runs past its page");
/// ```
struct error {
    errc code = errc::io;
    std::string message;
    std::array<std::uint8_t, 16> expected_fingerprint{};
    std::array<std::uint8_t, 16> found_fingerprint{};

    error() = default;
    error(errc c, std::string m = {}) : code(c), message(std::move(m)) {}

    /// The message, prefixed with what kind of failure it is.
    std::string what() const;
};

/// The exception every failure that ends the session is thrown as, in
/// builds with exceptions.
class exception : public std::runtime_error {
public:
    explicit exception(kladde::error e) : std::runtime_error(e.what()), error_(std::move(e)) {}
    kladde::error const& error() const noexcept { return error_; }

private:
    kladde::error error_;
};

/// A value or the error that prevented it: `std::expected<T, kladde::error>`
/// for C++20.
///
/// Opening and creating files return one, since their failures are ordinary
/// and the caller must handle them, with or without exceptions.
///
/// ```cpp
/// kladde::expected<int> parse(std::string_view s);
/// if (auto n = parse("12")) use(*n); else log(n.error().what());
/// ```
template <class T>
class [[nodiscard]] expected {
public:
    expected(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    expected(kladde::error e) : v_(std::in_place_index<1>, std::move(e)) {}
    expected(errc c) : v_(std::in_place_index<1>, kladde::error{c}) {}

    bool has_value() const noexcept { return v_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { check(); return std::get<0>(v_); }
    T const& value() const& { check(); return std::get<0>(v_); }
    T&& value() && { check(); return std::get<0>(std::move(v_)); }
    T& operator*() & { return std::get<0>(v_); }
    T const& operator*() const& { return std::get<0>(v_); }
    T&& operator*() && { return std::get<0>(std::move(v_)); }
    T* operator->() { return &std::get<0>(v_); }
    T const* operator->() const { return &std::get<0>(v_); }

    kladde::error const& error() const& { return std::get<1>(v_); }
    kladde::error&& error() && { return std::get<1>(std::move(v_)); }

private:
    void check() const;
    std::variant<T, kladde::error> v_;
};

/// `expected<void>`: success, or the error.
template <>
class [[nodiscard]] expected<void> {
public:
    expected() = default;
    expected(kladde::error e) : e_(std::move(e)), ok_(false) {}
    expected(errc c) : e_(kladde::error{c}), ok_(false) {}

    bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    void value() const;
    kladde::error const& error() const& { return e_; }
    kladde::error&& error() && { return std::move(e_); }

private:
    kladde::error e_;
    bool ok_ = true;
};

/// What `report` calls in a build without exceptions, before it returns:
/// show the failure to the user, say. The default handler prints to
/// standard error.
using error_handler = std::function<void(kladde::error const&)>;

/// Installs `handler` for builds without exceptions; returns the previous
/// one.
error_handler set_error_handler(error_handler handler);

/// Reports a failure that ends the session: throws `kladde::exception` in a
/// build with exceptions, and calls the installed handler otherwise. The
/// store has been poisoned before this is called, so every later mutation
/// fails too.
void report(kladde::error e);

/// The bytes of a fingerprint as 32 lowercase hexadecimal digits.
std::string to_hex(std::array<std::uint8_t, 16> const& bytes);

template <class T>
void expected<T>::check() const {
    if (!has_value()) {
#if KLADDE_EXCEPTIONS
        throw kladde::exception(std::get<1>(v_));
#else
        report(std::get<1>(v_));
        std::abort();
#endif
    }
}

inline void expected<void>::value() const {
    if (!ok_) {
#if KLADDE_EXCEPTIONS
        throw kladde::exception(e_);
#else
        report(e_);
        std::abort();
#endif
    }
}

}  // namespace kladde

/// Evaluates `expr`, an `expected`, and returns its error from the enclosing
/// function if it holds one: Rust's `?`.
#define KLADDE_TRY(expr)                                 \
    do {                                                 \
        auto&& kladde_try_result_ = (expr);              \
        if (!kladde_try_result_)                         \
            return std::move(kladde_try_result_).error(); \
    } while (false)

#define KLADDE_CONCAT_INNER(a, b) a##b
#define KLADDE_CONCAT(a, b) KLADDE_CONCAT_INNER(a, b)

/// Evaluates `expr`, an `expected<T>`, and declares `decl` from its value,
/// or returns its error from the enclosing function.
#define KLADDE_TRY_ASSIGN(decl, expr) \
    KLADDE_TRY_ASSIGN_IMPL(KLADDE_CONCAT(kladde_try_value_, __LINE__), decl, expr)
#define KLADDE_TRY_ASSIGN_IMPL(tmp, decl, expr) \
    auto tmp = (expr);                          \
    if (!tmp) return std::move(tmp).error();    \
    decl = std::move(*tmp)

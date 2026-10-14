// `std::string` as Text: UTF-8, then the byte 0xFF, which UTF-8 never
// contains.
//
// Text must be UTF-8. A `std::string` used as a byte buffer — file contents,
// a serialized message, a binary identifier — is not text, and storing one
// fails: hold bytes in a `std::vector<std::uint8_t>` instead.
#pragma once

#include <kladde/typed/core.hpp>

namespace kladde {

/// Whether `s` is valid UTF-8: shortest forms, no surrogates, nothing past
/// U+10FFFF. Such text never contains the byte 0xFF.
inline bool is_utf8(std::string_view s) {
    auto const* p = reinterpret_cast<unsigned char const*>(s.data());
    std::size_t n = s.size(), i = 0;
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len;
        std::uint32_t cp;
        if ((c >> 5) == 6) len = 2, cp = c & 0x1F;
        else if ((c >> 4) == 14) len = 3, cp = c & 0x0F;
        else if ((c >> 3) == 30) len = 4, cp = c & 0x07;
        else return false;
        if (i + len > n) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        std::uint32_t min = len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return false;
        i += len;
    }
    return true;
}

/// Whether byte position `pos` of `s` starts a character or ends `s`.
inline bool is_char_boundary(std::string_view s, std::size_t pos) {
    return pos == s.size() || (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) != 0x80);
}

inline bytes_view as_bytes(std::string_view s) {
    return {reinterpret_cast<std::uint8_t const*>(s.data()), s.size()};
}

/// The handle of a `std::string`: every edit is one splice of the text's
/// bytes, and the terminator stays where the text ends.
///
/// Positions are byte positions and must lie on character boundaries, and
/// inserted text must be UTF-8; otherwise the edit fails with
/// `errc::out_of_bounds`, recording nothing.
///
/// ```cpp
/// auto file = kladde::File<std::string>::create("note.kladde", "kladde").value();
/// auto text = file.edit();
/// text.append(" file");
/// text.replace(0, 6, "Kladde");
/// assert(file.get() == "Kladde file");
/// ```
class TextMut : public MutBase<std::string> {
public:
    using MutBase::MutBase;

    /// Replaces the whole text, recording only the range between the old
    /// and the new text's common prefix and suffix: setting a long text of
    /// which a few characters changed records a few bytes.
    void set(std::string text) {
        if (!valid(text)) return;
        auto& b = region_.backend();
        if (!check(b, splice_changed(region_, 0, as_bytes(*value_), as_bytes(text)))) return;
        *value_ = std::move(text);
    }
    TextMut& operator=(std::string text) {
        set(std::move(text));
        return *this;
    }
    /// Replaces `count` bytes at `pos` with `text`.
    void replace(std::size_t pos, std::size_t count, std::string_view text) {
        if (pos > value_->size()) return out_of_bounds();
        count = std::min(count, value_->size() - pos);
        if (!is_char_boundary(*value_, pos) || !is_char_boundary(*value_, pos + count))
            return out_of_bounds();
        if (!valid(text)) return;
        std::string fresh;
        fresh.reserve(value_->size() - count + text.size());  // may throw: before recording
        fresh.append(*value_, 0, pos).append(text).append(*value_, pos + count);
        if (!check(region_.backend(), region_.splice(pos, count, as_bytes(text)))) return;
        value_->swap(fresh);
    }
    void insert(std::size_t pos, std::string_view text) { replace(pos, 0, text); }
    void erase(std::size_t pos, std::size_t count = std::string::npos) { replace(pos, count, {}); }
    void append(std::string_view text) { replace(value_->size(), 0, text); }
    void clear() { replace(0, value_->size(), {}); }

private:
    bool valid(std::string_view text) {
        if (is_utf8(text)) return true;
        report(error{errc::out_of_bounds, "text that is not UTF-8"});
        return false;
    }
    void out_of_bounds() { report(error{errc::out_of_bounds, "a position inside a character or past the text"}); }
};

template <>
struct durable<std::string> {
    static constexpr std::optional<std::size_t> fixed_size = std::nullopt;
    static std::size_t encoded_len(std::string const& s) { return s.size() + 1; }
    static void encode(std::string const& s, Encoder& out) {
        assert(is_utf8(s) && "text must be UTF-8");
        out.insert(out.end(), s.begin(), s.end());
        out.push_back(0xFF);
    }
    static bool decode(Decoder& in, std::string& s) {
        auto b = in.until_terminator();
        if (!b) return false;
        s.assign(reinterpret_cast<char const*>(b->data()), b->size());
        return is_utf8(s) || in.fail("text that is not UTF-8");
    }
    static schema::TypeRef describe(SchemaBuilder& b) {
        return b.describe(&type_key<std::string>,
                          [](SchemaBuilder&) -> schema::TypeDescriptor { return schema::Text{}; });
    }
    using mut = TextMut;
};

}  // namespace kladde

// The text form of a table that cross-implementation tests compare; the
// format is documented at `dump` in schema.hpp, and is lossless: it shows
// every detail the canonical encoding holds, in canonical order.
#include "internal.hpp"

#include <cstdio>

namespace kladde::schema {

namespace {

void put_quoted(std::string& out, std::string const& s) {
    out += '"';
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c == 0x7f) {
            char buf[5];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += ch;
        }
    }
    out += '"';
}

void put_ref(std::string& out, TypeRef r) { out += '#' + std::to_string(r.index); }

char const* primitive_name(Primitive p) {
    switch (p) {
        case Primitive::u8: return "u8";
        case Primitive::u16: return "u16";
        case Primitive::u32: return "u32";
        case Primitive::u64: return "u64";
        case Primitive::i8: return "i8";
        case Primitive::i16: return "i16";
        case Primitive::i32: return "i32";
        case Primitive::i64: return "i64";
        case Primitive::f32: return "f32";
        case Primitive::f64: return "f64";
        case Primitive::boolean: return "bool";
        case Primitive::character: return "char";
    }
    return nullptr;
}

struct Line {
    std::string& out;

    void operator()(Primitive p) {
        if (auto const* name = primitive_name(p)) {
            out += name;
        } else {
            out += "primitive " + std::to_string(static_cast<unsigned>(p));
        }
    }
    void operator()(Struct const& s) {
        out += "struct ";
        put_quoted(out, s.name);
        if (s.fields.empty()) {
            out += " {}";
            return;
        }
        out += " { ";
        for (std::size_t i = 0; i < s.fields.size(); ++i) {
            if (i > 0) out += ", ";
            put_quoted(out, s.fields[i].name);
            out += ": ";
            put_ref(out, s.fields[i].type);
        }
        out += " }";
    }
    void operator()(Enum const& e) {
        out += "enum ";
        put_quoted(out, e.name);
        out += " width " + std::to_string(e.discriminant_width);
        if (e.variants.empty()) {
            out += " {}";
            return;
        }
        out += " { ";
        bool first = true;
        for (auto const* v : detail::canonical_variants(e.variants)) {
            if (!first) out += ", ";
            first = false;
            out += std::to_string(v->discriminant) + ' ';
            put_quoted(out, v->name);
            out += ": ";
            put_ref(out, v->payload);
        }
        out += " }";
    }
    void operator()(Opaque const& o) {
        out += "opaque ";
        put_quoted(out, o.library_name);
        out += ' ';
        put_quoted(out, o.type_name);
        out += ' ' + std::to_string(o.version.major) + '.' + std::to_string(o.version.minor) + '.' +
               std::to_string(o.version.patch);
        out += o.fixed_size ? " size " + std::to_string(*o.fixed_size) : std::string(" size -");
        out += " <";
        for (std::size_t i = 0; i < o.parameters.size(); ++i) {
            if (i > 0) out += ", ";
            put_ref(out, o.parameters[i]);
        }
        out += '>';
    }
    void operator()(Array const& a) {
        out += "array ";
        put_ref(out, a.element);
        out += " len " + std::to_string(a.length);
    }
    void operator()(Sequence const& s) {
        out += "sequence ";
        put_ref(out, s.element);
    }
    void operator()(Map const& m) {
        out += "map ";
        put_ref(out, m.key);
        out += " -> ";
        put_ref(out, m.value);
    }
    void operator()(Text const&) { out += "text"; }
    void operator()(Fixed const& f) {
        out += "fixed ";
        put_ref(out, f.inner);
    }
};

}  // namespace

std::string dump(TypeTable const& table) {
    std::string out;
    auto const& ds = table.descriptors();
    for (std::size_t i = 0; i < ds.size(); ++i) {
        out += '[' + std::to_string(i) + "] ";
        std::visit(Line{out}, ds[i]);
        out += '\n';
    }
    return out;
}

}  // namespace kladde::schema

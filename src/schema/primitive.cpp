#include <kladde/schema.hpp>

namespace kladde::schema {

std::optional<std::uint64_t> fixed_size(Primitive p) {
    switch (p) {
        case Primitive::u8:
        case Primitive::i8:
        case Primitive::boolean: return 1;
        case Primitive::f32: return 4;
        case Primitive::f64: return 8;
        default: return std::nullopt;
    }
}

std::uint64_t fixed_form_size(Primitive p) {
    switch (p) {
        case Primitive::u8:
        case Primitive::i8:
        case Primitive::boolean: return 1;
        case Primitive::u16:
        case Primitive::i16: return 2;
        case Primitive::u32:
        case Primitive::i32:
        case Primitive::f32:
        case Primitive::character: return 4;
        case Primitive::u64:
        case Primitive::i64:
        case Primitive::f64: return 8;
    }
    return 0;
}

}  // namespace kladde::schema

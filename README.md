<img src="https://kladde-dev.github.io/static/logo.svg" alt="kladde logo" width="80" align="right">

# kladde-cpp

The C++ implementation of [kladde](https://kladde-dev.github.io/): durable data structures that you mutate in memory, and it's on disk.

**Status: not yet usable; a proof of concept in the making, to test the specification.**
It is being written almost entirely by an AI coding agent from the [specification and design documents](https://kladde-dev.github.io/), and the file format it writes is not frozen.

Its design is proposed in [A C++ implementation](https://kladde-dev.github.io/drafts/cpp/) and recorded as built in [kladde-cpp](https://kladde-dev.github.io/cpp/); the first application to use it is [inkscape-kladde](https://github.com/kladde-dev/inkscape-kladde), a fork of Inkscape without a save button.

```cpp
#include <kladde/kladde.hpp>

struct Contact {
    std::string email;
    bool starred = false;
};
KLADDE_STRUCT(Contact, email, starred)

auto file = kladde::File<Contact>::create("contacts.kladde", Contact{.email = "ada@example.com"}).value();
file.edit().starred().set(true);  // durable when it returns
```

## Building

C++20 and CMake, with no required dependencies; the tests need GoogleTest.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DKLADDE_SANITIZE=ON
cmake --build build
./build/kladde-schema-tests && ./build/kladde-store-tests && ./build/kladde-typed-tests
```

| directory | holds |
| --- | --- |
| `include/kladde/schema.hpp`, `src/schema` | type descriptors, their canonical encoding, and fingerprints |
| `include/kladde/store.hpp`, `src/store` | the store: the file format, the journal, the flush and consolidation, ported from kladde-rs |
| `include/kladde/typed`, `include/kladde/file.hpp` | the header-only typed layer: durable types, their `Mut` handles, `kladde::List` and `kladde::Ref`, and `kladde::File` |
| `tools` | `kladde-store-dump`, which prints a file's sequence, fingerprint and schema, and `kladde-cross`, the C++ side of the cross-implementation test |

In a project, add the directory with `add_subdirectory` and link `kladde::kladde`.

## License

Available under your choice of the [MIT](LICENSE-MIT), [Apache 2.0](LICENSE-APACHE), or [Boost Software License 1.0](LICENSE-BOOST) (SPDX: `MIT OR Apache-2.0 OR BSL-1.0`).

Unless you explicitly state otherwise, any contribution you intentionally submit for inclusion in this repository shall be licensed as above, without any additional terms or conditions.

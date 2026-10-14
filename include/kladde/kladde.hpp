// Everything an application needs: durable types, their handles, and
// `kladde::File`.
//
// The typed layer is header-only; the schema and the store are compiled,
// in the library `kladde::kladde`.
#pragma once

#include <kladde/file.hpp>
#include <kladde/typed/core.hpp>
#include <kladde/typed/list.hpp>
#include <kladde/typed/map.hpp>
#include <kladde/typed/scalars.hpp>
#include <kladde/typed/std_types.hpp>
#include <kladde/typed/structs.hpp>
#include <kladde/typed/text.hpp>
#include <kladde/typed/vector.hpp>

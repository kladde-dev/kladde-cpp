#include "util.hpp"

#include <cstdio>
#include <cstdlib>

namespace kladde::store {

void fail_check(char const* msg, char const* file, int line) {
    std::fprintf(stderr, "kladde store invariant violated at %s:%d: %s\n", file, line, msg);
    std::abort();
}

}  // namespace kladde::store

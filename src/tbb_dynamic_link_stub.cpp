// SPDX-License-Identifier: MIT
// Stub for TBB dynamic_link on Windows cross-compilation targets.
// dynamic_link.cpp requires Softpub.h (Windows SDK) which is not
// available in Zig's cross toolchain. Since TBB is statically linked
// and __TBB_DYNAMIC_LOAD_ENABLED=0, these symbols are never actually
// called at runtime — this stub just satisfies the linker.

#include <cstdint>

struct HINSTANCE__;

namespace tbb { namespace detail { namespace r1 {

struct dynamic_link_descriptor {};

bool dynamic_link(const char*, const dynamic_link_descriptor*,
                  uint64_t, HINSTANCE__**, int) { return false; }

void dynamic_unlink(HINSTANCE__*) {}
void dynamic_unlink_all() {}

}}} // namespace tbb::detail::r1

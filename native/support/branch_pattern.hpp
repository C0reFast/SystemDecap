#pragma once

#include "../runtime.hpp"

namespace sdc {

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, optimize("no-if-conversion,no-tree-vectorize")))
#else
__attribute__((noinline))
#endif
static std::uint64_t branch_loop(const volatile std::uint8_t *pattern, std::size_t count,
                          std::size_t passes) {
  std::uint64_t value = 1;
  for (std::size_t pass = 0; pass < passes; ++pass) {
    for (std::size_t index = 0; index < count; ++index) {
      if (pattern[index]) {
        asm volatile("" ::: "memory");
        value = value * 3 + 1;
      } else {
        asm volatile("" ::: "memory");
        value = value * 3 - 1;
      }
    }
  }
  return value;
}


}  // namespace sdc

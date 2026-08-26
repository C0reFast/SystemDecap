#pragma once

#include "../runtime.hpp"

namespace sdc {

inline double chase_chain(std::byte *base, std::size_t iterations) {
  std::uint32_t current = 0;
  const auto begin = Clock::now();
  for (std::size_t index = 0; index < iterations; ++index) {
    current = *reinterpret_cast<volatile std::uint32_t *>(base + current);
  }
  const auto end = Clock::now();
  global_sink = global_sink ^ current;
  return seconds_between(begin, end) * 1e9 / static_cast<double>(iterations);
}

inline double median_double(std::vector<double> values) {
  if (values.empty()) return 0.0;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

inline void build_random_chain(std::byte *base, std::size_t bytes, std::size_t stride,
                        std::mt19937 &random) {
  const std::size_t points = std::max<std::size_t>(2, bytes / stride);
  std::vector<std::uint32_t> order(points);
  std::iota(order.begin(), order.end(), 0U);
  std::shuffle(order.begin(), order.end(), random);
  for (std::size_t index = 0; index < points; ++index) {
    const auto here = static_cast<std::size_t>(order[index]) * stride;
    const auto next = static_cast<std::size_t>(order[(index + 1) % points]) * stride;
    *reinterpret_cast<std::uint32_t *>(base + here) = static_cast<std::uint32_t>(next);
  }
}

inline void build_random_page_chain(std::byte *base, std::size_t pages,
                             std::size_t page_size, std::mt19937 &random) {
  const std::size_t cache_line = 64;
  const std::size_t set_spread = std::max<std::size_t>(1, page_size / cache_line);
  std::vector<std::uint32_t> addresses(pages);
  for (std::size_t page = 0; page < pages; ++page) {
    const std::size_t offset = (page % set_spread) * cache_line;
    addresses[page] = static_cast<std::uint32_t>(page * page_size + offset);
  }
  std::shuffle(addresses.begin(), addresses.end(), random);
  for (std::size_t index = 0; index < addresses.size(); ++index) {
    *reinterpret_cast<std::uint32_t *>(base + addresses[index]) =
        addresses[(index + 1) % addresses.size()];
  }
}


}  // namespace sdc

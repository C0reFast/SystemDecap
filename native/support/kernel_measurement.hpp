#pragma once

#include "../runtime.hpp"

namespace sdc {

using GeneratedFunction = void (*)();

struct KernelMeasurement {
  double seconds = 0.0;
  std::uint64_t counter_ticks = 0;
  PerfCounts perf;
};

template <typename Function>
KernelMeasurement measure_kernel(Function &&function) {
  function();
  PerfGroup core(PerfGroupKind::Core);
  PerfGroup branch(PerfGroupKind::Branch);
  PerfGroup cache(PerfGroupKind::Cache);
  const auto wall_begin = Clock::now();
  const auto tick_begin = cycle_counter();
  core.start();
  branch.start();
  cache.start();
  function();
  auto cache_counts = cache.stop();
  auto branch_counts = branch.stop();
  auto counts = core.stop();
  const auto tick_end = cycle_counter();
  const auto wall_end = Clock::now();
  if (branch_counts.available && branch_counts.branch_counters_available) {
    counts.branch_counters_available = true;
    counts.branches = branch_counts.branches;
    counts.branch_misses = branch_counts.branch_misses;
  }
  if (cache_counts.available && cache_counts.cache_counters_available) {
    counts.cache_counters_available = true;
    counts.cache_references = cache_counts.cache_references;
    counts.cache_misses = cache_counts.cache_misses;
  }
  return {seconds_between(wall_begin, wall_end), tick_end - tick_begin, std::move(counts)};
}


}  // namespace sdc

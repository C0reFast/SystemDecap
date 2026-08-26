#include "../probe.hpp"

namespace sdc {
namespace {

enum class StoreForwardCase { Exact64, Partial32To64, OverlapByOne, SplitLine };

std::pair<double, double> store_forward_measurement(StoreForwardCase test_case,
                                                     std::size_t iterations) {
  alignas(128) std::array<std::byte, 256> storage{};
  auto *base = storage.data() + (test_case == StoreForwardCase::SplitLine ? 60 : 64);
  std::uint64_t value = 0x123456789abcdef0ULL;
  const auto tick_begin = cycle_counter();
  const auto wall_begin = Clock::now();
#if defined(__x86_64__)
  if (test_case == StoreForwardCase::Partial32To64) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("movl %k[value], (%[address])\n\tmovq (%[address]), %[value]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  } else if (test_case == StoreForwardCase::OverlapByOne) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("movq %[value], (%[store])\n\tmovq (%[load]), %[value]"
                   : [value] "+r"(value)
                   : [store] "r"(base), [load] "r"(base + 1) : "memory");
    }
  } else {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("movq %[value], (%[address])\n\tmovq (%[address]), %[value]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  }
#elif defined(__aarch64__)
  if (test_case == StoreForwardCase::Partial32To64) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("str %w[value], [%[address]]\n\tldr %x[value], [%[address]]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  } else if (test_case == StoreForwardCase::OverlapByOne) {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("str %x[value], [%[store]]\n\tldr %x[value], [%[load]]"
                   : [value] "+r"(value)
                   : [store] "r"(base), [load] "r"(base + 1) : "memory");
    }
  } else {
    for (std::size_t index = 0; index < iterations; ++index) {
      asm volatile("str %x[value], [%[address]]\n\tldr %x[value], [%[address]]"
                   : [value] "+r"(value) : [address] "r"(base) : "memory");
    }
  }
#endif
  const double seconds = seconds_between(wall_begin, Clock::now());
  const auto ticks = cycle_counter() - tick_begin;
  global_sink = global_sink ^ value;
  return {seconds * 1e9 / static_cast<double>(iterations),
          static_cast<double>(ticks) / static_cast<double>(iterations)};
}

void benchmark_store_forwarding(const Options &options, int cpu,
                                std::vector<Observation> &observations) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t iterations = options.profile == "quick" ? 1000000 : 5000000;
  struct Case {
    StoreForwardCase value;
    const char *name;
    int store_bytes;
    int load_bytes;
    int store_offset;
    int load_offset;
  };
  const std::array<Case, 4> cases{{
      {StoreForwardCase::Exact64, "exact-8-to-8", 8, 8, 0, 0},
      {StoreForwardCase::Partial32To64, "partial-4-to-8", 4, 8, 0, 0},
      {StoreForwardCase::OverlapByOne, "overlap-offset-1", 8, 8, 0, 1},
      {StoreForwardCase::SplitLine, "split-cache-line", 8, 8, 60, 60},
  }};
  for (const auto &test_case : cases) {
    const auto [nanoseconds, ticks] = store_forward_measurement(test_case.value, iterations);
    const Labels labels{{"case", test_case.name},
                        {"store_bytes", std::to_string(test_case.store_bytes)},
                        {"load_bytes", std::to_string(test_case.load_bytes)},
                        {"store_offset", std::to_string(test_case.store_offset)},
                        {"load_offset", std::to_string(test_case.load_offset)},
                        {"cpu", std::to_string(cpu)}};
    add_observation(observations, "store_forwarding", "store_load_latency",
                    nanoseconds, "ns/pair", "medium",
                    "dependent store followed by overlapping load", labels);
    add_observation(observations, "store_forwarding", "store_load_counter_ticks",
                    ticks, "counter-ticks/pair", "low",
                    "platform counter delta for dependent store/load pair", labels);
  }
}

void run(ProbeContext &context) {
  benchmark_store_forwarding(context.options, context.primary_cpu,
                               context.observations);
}

}  // namespace

ProbeDefinition store_forwarding_probe() {
  return {"store-forwarding", "store-to-load forwarding", "Store-to-load forwarding 对齐代价", run};
}

}  // namespace sdc

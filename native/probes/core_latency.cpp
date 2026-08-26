#include "../probe.hpp"

namespace sdc {
namespace {

double ping_pong_latency(int cpu_a, int cpu_b, std::size_t rounds) {
  alignas(128) std::atomic<int> turn{0};
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  double elapsed = 0.0;
  std::thread first([&] {
    pin_to_cpu(cpu_a);
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    const auto begin = Clock::now();
    for (std::size_t round = 0; round < rounds; ++round) {
      while (turn.load(std::memory_order_acquire) != 0) cpu_relax();
      turn.store(1, std::memory_order_release);
      while (turn.load(std::memory_order_acquire) != 0) cpu_relax();
    }
    elapsed = seconds_between(begin, Clock::now());
  });
  std::thread second([&] {
    pin_to_cpu(cpu_b);
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    for (std::size_t round = 0; round < rounds; ++round) {
      while (turn.load(std::memory_order_acquire) != 1) cpu_relax();
      turn.store(0, std::memory_order_release);
    }
  });
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  first.join();
  second.join();
  return elapsed * 1e9 / static_cast<double>(rounds) / 2.0;
}

void benchmark_core_latency(const Options &options, const std::vector<CpuInfo> &cpus,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  if (cpus.size() < 2) {
    warnings.push_back("已跳过核间延迟：当前进程亲和性范围内不足两个 CPU");
    return;
  }
  using Pair = std::tuple<CpuInfo, CpuInfo, std::string>;
  std::vector<Pair> pairs;
  if (options.profile == "deep") {
    for (std::size_t left = 0; left < cpus.size(); ++left) {
      for (std::size_t right = left + 1; right < cpus.size(); ++right) {
        pairs.emplace_back(cpus[left], cpus[right], "logical-cpu");
      }
    }
  } else if (options.profile == "standard") {
    const auto physical = physical_cpus(cpus);
    for (std::size_t left = 0; left < physical.size(); ++left) {
      for (std::size_t right = left + 1; right < physical.size(); ++right) {
        pairs.emplace_back(physical[left], physical[right], "physical-core");
      }
    }

    // Physical-core representatives intentionally omit SMT siblings. Keep one
    // additional pair so the relation summary still describes the SMT path.
    for (std::size_t left = 0; left < cpus.size(); ++left) {
      bool found = false;
      for (std::size_t right = left + 1; right < cpus.size(); ++right) {
        if (cpu_relation(cpus[left], cpus[right]) == "smt-sibling") {
          pairs.emplace_back(cpus[left], cpus[right], "representative");
          found = true;
          break;
        }
      }
      if (found) break;
    }
  } else {
    std::set<std::string> represented;
    for (std::size_t left = 0; left < cpus.size(); ++left) {
      for (std::size_t right = left + 1; right < cpus.size(); ++right) {
        const auto relation = cpu_relation(cpus[left], cpus[right]);
        if (represented.insert(relation).second) {
          pairs.emplace_back(cpus[left], cpus[right], "representative");
        }
      }
    }
  }

  const std::size_t pair_count = std::max<std::size_t>(1, pairs.size());
  const std::size_t rounds = options.profile == "smoke" ? 2000
      : options.profile == "quick" ? 20000
      : options.profile == "deep"
          ? std::clamp<std::size_t>(5000000 / pair_count, 5000, 100000)
          : std::clamp<std::size_t>(2000000 / pair_count, 2000, 50000);
  for (const auto &[left, right, matrix_scope] : pairs) {
    const double latency = ping_pong_latency(left.cpu, right.cpu, rounds);
    add_observation(observations, "core_latency", "cacheline_handoff_latency", latency,
                    "ns/one-way", "high", "release/acquire cache-line ping-pong",
                    {{"cpu_a", std::to_string(left.cpu)},
                     {"cpu_b", std::to_string(right.cpu)},
                     {"node_a", std::to_string(left.node)},
                     {"node_b", std::to_string(right.node)},
                     {"relation", cpu_relation(left, right)},
                     {"matrix_scope", matrix_scope},
                     {"rounds", std::to_string(rounds)}});
  }
}

void run(ProbeContext &context) {
  benchmark_core_latency(context.options, context.cpus, context.observations,
                           context.warnings);
}

}  // namespace

ProbeDefinition core_latency_probe() {
  return {"core-latency", "core-to-core cache-line latency", "CPU×CPU 缓存行传递延迟矩阵", run};
}

}  // namespace sdc

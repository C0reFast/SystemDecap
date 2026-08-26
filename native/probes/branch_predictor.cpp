#include "../probe.hpp"
#include "../support/branch_pattern.hpp"
#include "../support/kernel_measurement.hpp"

namespace sdc {
namespace {

void benchmark_branches(const Options &options, std::vector<Observation> &observations,
                        int cpu) {
  pin_to_cpu(cpu);
  constexpr std::size_t count = 65536;
  const std::size_t passes = options.profile == "smoke" ? 2
      : options.profile == "quick" ? 32
                                  : 128;
  std::mt19937 random(options.seed ^ 0xB12A4C4U);
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> patterns;
  patterns.emplace_back("always-taken", std::vector<std::uint8_t>(count, 1));
  patterns.emplace_back("alternating", std::vector<std::uint8_t>(count, 0));
  patterns.emplace_back("random", std::vector<std::uint8_t>(count, 0));
  for (std::size_t index = 0; index < count; ++index) {
    patterns[1].second[index] = static_cast<std::uint8_t>(index & 1U);
    patterns[2].second[index] = static_cast<std::uint8_t>(random() & 1U);
  }
  for (auto &[name, pattern] : patterns) {
    const auto measurement = measure_kernel([&] {
      global_sink = global_sink ^ branch_loop(pattern.data(), pattern.size(), passes);
    });
    const double branches = static_cast<double>(count * passes);
    const Labels labels{{"pattern", name}};
    add_observation(observations, "branch", "time_per_branch",
                    measurement.seconds * 1e9 / branches, "ns/branch", "medium",
                    "forced scalar data-dependent branch loop", labels);
    if (measurement.perf.available && measurement.perf.branch_counters_available &&
        measurement.perf.branches > 0.0) {
      add_observation(observations, "branch", "miss_rate",
                      100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                      "%", "high", "perf branch misses / branch instructions", labels);
      add_observation(observations, "branch", "ipc",
                      measurement.perf.instructions / measurement.perf.cycles,
                      "instructions/cycle", "high", "perf retired instructions / core cycles", labels);
    }
  }
}

void run(ProbeContext &context) {
  benchmark_branches(context.options, context.observations, context.primary_cpu);
}

}  // namespace

ProbeDefinition branch_predictor_probe() {
  return {"branch-predictor", "branch predictor", "可预测与随机分支代价", run};
}

}  // namespace sdc

#include "../probe.hpp"
#include "../support/branch_pattern.hpp"
#include "../support/kernel_measurement.hpp"

namespace sdc {
namespace {

void benchmark_btb_capacity(const Options &options, int cpu,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 2048
      : options.profile == "deep" ? 32768 : 8192;
  const std::size_t target_branches = options.profile == "quick" ? 500000 :
      options.profile == "deep" ? 4000000 : 2000000;
  try {
    for (const std::string branch_type : {"unconditional", "conditional-taken"}) {
      for (const std::size_t spacing : {std::size_t{4}, std::size_t{8}, std::size_t{16},
                                        std::size_t{32}, std::size_t{64}}) {
        for (const std::size_t count : power_sizes(16, maximum)) {
#if defined(__x86_64__)
        const std::size_t prefix = branch_type == "conditional-taken" ? 2 : 0;
        ExecutableBuffer code(prefix + count * spacing + 4);
        std::memset(code.bytes(), 0x90, code.size());
        if (prefix != 0) {
          code.bytes()[0] = std::byte{0x31};
          code.bytes()[1] = std::byte{0xc0};
        }
        for (std::size_t index = 0; index < count; ++index) {
          const std::size_t offset = prefix + index * spacing;
          code.bytes()[offset] = branch_type == "conditional-taken"
              ? std::byte{0x74} : std::byte{0xeb};
          code.bytes()[offset + 1] = static_cast<std::byte>(spacing - 2);
        }
        code.bytes()[prefix + count * spacing] = std::byte{0xc3};
#elif defined(__aarch64__)
        const std::size_t prefix = branch_type == "conditional-taken" ? 4 : 0;
        ExecutableBuffer code(prefix + count * spacing + 4);
        constexpr std::uint32_t nop = 0xd503201fU;
        constexpr std::uint32_t ret = 0xd65f03c0U;
        for (std::size_t offset = 0; offset < code.size() - 4; offset += 4)
          std::memcpy(code.bytes() + offset, &nop, sizeof(nop));
        if (prefix != 0) {
          constexpr std::uint32_t compare_zero = 0xeb1f03ffU;
          std::memcpy(code.bytes(), &compare_zero, sizeof(compare_zero));
        }
        const std::uint32_t distance = static_cast<std::uint32_t>(spacing / 4);
        const std::uint32_t branch = branch_type == "conditional-taken"
            ? 0x54000000U | (distance << 5) : 0x14000000U | distance;
        for (std::size_t index = 0; index < count; ++index)
          std::memcpy(code.bytes() + prefix + index * spacing, &branch, sizeof(branch));
        std::memcpy(code.bytes() + prefix + count * spacing, &ret, sizeof(ret));
#endif
        code.seal();
        const auto function = code.function_at<GeneratedFunction>();
        const std::size_t passes = std::max<std::size_t>(1, target_branches / count);
        const auto measurement = measure_kernel([&] {
          for (std::size_t pass = 0; pass < passes; ++pass) function();
        });
        const double branches = static_cast<double>(count) * passes;
        const Labels labels{{"cpu", std::to_string(cpu)},
                            {"branch_type", branch_type},
                            {"branch_count", std::to_string(count)},
                            {"spacing_bytes", std::to_string(spacing)},
                            {"code_bytes", std::to_string(count * spacing)}};
        add_observation(observations, "branch_structure", "btb_branch_latency",
                        measurement.seconds * 1e9 / branches, "ns/branch", "low",
                        "generated taken direct branches with independently varied footprint", labels);
        add_observation(observations, "branch_structure", "btb_counter_ticks",
                        static_cast<double>(measurement.counter_ticks) / branches,
                        "counter-ticks/branch", "low",
                        "platform counter ticks per generated taken branch", labels);
        if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
          add_observation(observations, "branch_structure", "btb_miss_rate",
                          100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                          "%", "medium", "generic perf branch misses during BTB footprint sweep",
                          labels);
        }
      }
    }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("BTB 容量探针提前停止：") + error.what());
  }
}

void benchmark_return_stack(const Options &options, int cpu,
                            std::vector<Observation> &observations,
                            std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 64 : 256;
  const std::size_t target_returns = options.profile == "quick" ? 250000 : 1000000;
  try {
    for (const std::size_t depth : power_sizes(1, maximum)) {
#if defined(__x86_64__)
      constexpr std::size_t slot = 6;
      ExecutableBuffer code(depth * slot + 1);
      for (std::size_t index = 0; index < depth; ++index) {
        const std::size_t offset = index * slot;
        code.bytes()[offset] = std::byte{0xe8};
        const std::int32_t displacement = 1;
        std::memcpy(code.bytes() + offset + 1, &displacement, sizeof(displacement));
        code.bytes()[offset + 5] = std::byte{0xc3};
      }
      code.bytes()[depth * slot] = std::byte{0xc3};
#elif defined(__aarch64__)
      constexpr std::size_t slot = 16;
      ExecutableBuffer code(depth * slot + 4);
      constexpr std::uint32_t save_frame_and_lr = 0xa9bf7bfdU;
      constexpr std::uint32_t call_next = 0x94000003U;
      constexpr std::uint32_t restore_frame_and_lr = 0xa8c17bfdU;
      constexpr std::uint32_t ret = 0xd65f03c0U;
      for (std::size_t index = 0; index < depth; ++index) {
        const std::size_t offset = index * slot;
        std::memcpy(code.bytes() + offset, &save_frame_and_lr, sizeof(save_frame_and_lr));
        std::memcpy(code.bytes() + offset + 4, &call_next, sizeof(call_next));
        std::memcpy(code.bytes() + offset + 8, &restore_frame_and_lr,
                    sizeof(restore_frame_and_lr));
        std::memcpy(code.bytes() + offset + 12, &ret, sizeof(ret));
      }
      std::memcpy(code.bytes() + depth * slot, &ret, sizeof(ret));
#endif
      code.seal();
      const auto function = code.function_at<GeneratedFunction>();
      const std::size_t returns_per_call = depth + 1;
      const std::size_t passes = std::max<std::size_t>(1, target_returns / returns_per_call);
      const auto measurement = measure_kernel([&] {
        for (std::size_t pass = 0; pass < passes; ++pass) function();
      });
      const double returns = static_cast<double>(returns_per_call) * passes;
      const Labels labels{{"cpu", std::to_string(cpu)},
                          {"depth", std::to_string(depth)},
                          {"unique_return_addresses", std::to_string(depth)}};
      add_observation(observations, "branch_structure", "return_stack_latency",
                      measurement.seconds * 1e9 / returns, "ns/return", "low",
                      "generated nested calls with a unique return address at each depth", labels);
      if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
        add_observation(observations, "branch_structure", "return_stack_miss_rate",
                        100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                        "%", "medium", "generic perf branch misses during nested call/return chain",
                        labels);
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("返回地址栈探针提前停止：") + error.what());
  }
}

void benchmark_indirect_targets(const Options &options, int cpu,
                                std::vector<Observation> &observations,
                                std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  const std::size_t maximum = options.profile == "quick" ? 512
      : options.profile == "deep" ? 16384 : 4096;
  const std::size_t target_calls = options.profile == "quick" ? 250000 :
      options.profile == "deep" ? 2000000 : 1000000;
  constexpr std::size_t slot = 16;
  try {
    ExecutableBuffer code(maximum * slot);
#if defined(__x86_64__)
    std::memset(code.bytes(), 0x90, code.size());
    for (std::size_t index = 0; index < maximum; ++index)
      code.bytes()[index * slot] = std::byte{0xc3};
#elif defined(__aarch64__)
    constexpr std::uint32_t nop = 0xd503201fU;
    constexpr std::uint32_t ret = 0xd65f03c0U;
    for (std::size_t offset = 0; offset < code.size(); offset += 4)
      std::memcpy(code.bytes() + offset, &nop, sizeof(nop));
    for (std::size_t index = 0; index < maximum; ++index)
      std::memcpy(code.bytes() + index * slot, &ret, sizeof(ret));
#endif
    code.seal();
    for (const std::size_t count : power_sizes(1, maximum)) {
      std::vector<GeneratedFunction> targets;
      targets.reserve(count);
      for (std::size_t index = 0; index < count; ++index)
        targets.push_back(code.function_at<GeneratedFunction>(index * slot));
      std::vector<std::size_t> order(count);
      std::iota(order.begin(), order.end(), 0);
      std::mt19937 random(options.seed ^ static_cast<unsigned>(count));
      std::shuffle(order.begin(), order.end(), random);
      const std::size_t rounds = std::max<std::size_t>(1, target_calls / count);
      const auto measurement = measure_kernel([&] {
        for (std::size_t round = 0; round < rounds; ++round)
          for (const std::size_t index : order) targets[index]();
      });
      const double calls = static_cast<double>(count) * rounds;
      const Labels labels{{"cpu", std::to_string(cpu)},
                          {"target_count", std::to_string(count)},
                          {"target_spacing_bytes", std::to_string(slot)}};
      add_observation(observations, "branch_structure", "indirect_call_latency",
                      measurement.seconds * 1e9 / calls, "ns/call", "low",
                      "one indirect call site visits a shuffled set of generated return targets",
                      labels);
      if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
        add_observation(observations, "branch_structure", "indirect_call_miss_rate",
                        100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                        "%", "medium", "generic perf branch misses during indirect target sweep",
                        labels);
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("间接分支目标容量探针提前停止：") + error.what());
  }
}

void benchmark_branch_history(const Options &options, int cpu,
                              std::vector<Observation> &observations) {
  if (options.profile == "smoke") return;
  pin_to_cpu(cpu);
  constexpr std::size_t count = 32768;
  const std::size_t maximum = options.profile == "quick" ? 256
      : options.profile == "deep" ? 16384 : 4096;
  const std::size_t target_branches = options.profile == "quick" ? 500000 : 2000000;
  for (const std::size_t period : power_sizes(1, maximum)) {
    std::vector<std::uint8_t> seed_pattern(period, 1);
    if (period > 1) {
      std::mt19937 random(options.seed ^ static_cast<unsigned>(period * 0x9e3779b1U));
      for (auto &value : seed_pattern) value = static_cast<std::uint8_t>(random() & 1U);
      seed_pattern.front() = 1;
      seed_pattern[period / 2] = 0;
    }
    std::vector<std::uint8_t> pattern(count);
    for (std::size_t index = 0; index < count; ++index)
      pattern[index] = seed_pattern[index % period];
    const std::size_t passes = std::max<std::size_t>(1, target_branches / count);
    const auto measurement = measure_kernel([&] {
      global_sink = global_sink ^ branch_loop(pattern.data(), pattern.size(), passes);
    });
    const double branches = static_cast<double>(count) * passes;
    const Labels labels{{"cpu", std::to_string(cpu)}, {"history_period", std::to_string(period)}};
    add_observation(observations, "branch_structure", "history_period_latency",
                    measurement.seconds * 1e9 / branches, "ns/branch", "low",
                    "scalar conditional branch over a repeating pseudo-random direction period",
                    labels);
    if (measurement.perf.branch_counters_available && measurement.perf.branches > 0.0) {
      add_observation(observations, "branch_structure", "history_period_miss_rate",
                      100.0 * measurement.perf.branch_misses / measurement.perf.branches,
                      "%", "medium", "generic perf branch misses during history-period sweep",
                      labels);
    }
  }
}

void benchmark_branch_structures(const Options &options, int cpu,
                                 std::vector<Observation> &observations,
                                 std::vector<std::string> &warnings) {
  benchmark_btb_capacity(options, cpu, observations, warnings);
  benchmark_branch_history(options, cpu, observations);
  benchmark_return_stack(options, cpu, observations, warnings);
  benchmark_indirect_targets(options, cpu, observations, warnings);
}

void run(ProbeContext &context) {
  benchmark_branch_structures(context.options, context.primary_cpu,
                               context.observations, context.warnings);
}

}  // namespace

ProbeDefinition branch_structures_probe() {
  return {"branch-structures", "BTB, history, return-stack and indirect-target sweeps", "BTB、方向历史、返回栈与间接目标压力", run};
}

}  // namespace sdc

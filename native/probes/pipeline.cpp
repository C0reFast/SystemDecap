#include "../probe.hpp"
#include "../support/kernel_measurement.hpp"

namespace sdc {
namespace {

void nop_kernel(std::size_t iterations) {
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    asm volatile(".rept 128\n\tnop\n\t.endr" ::: "memory");
  }
}

void add_one_chain(std::size_t iterations) {
  std::uint64_t a0 = 1;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 64\n\taddq $1, %[a0]\n\t.endr"
                 : [a0] "+r"(a0));
#elif defined(__aarch64__)
    asm volatile(".rept 64\n\tadd %[a0], %[a0], #1\n\t.endr"
                 : [a0] "+r"(a0));
#else
    for (int index = 0; index < 64; ++index) a0 += 1;
#endif
  }
  global_sink = global_sink ^ a0;
}

void add_four_chains(std::size_t iterations) {
  std::uint64_t a0 = 1, a1 = 3, a2 = 5, a3 = 7;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 16\n\t"
                 "addq $1, %[a0]\n\taddq $1, %[a1]\n\t"
                 "addq $1, %[a2]\n\taddq $1, %[a3]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3));
#elif defined(__aarch64__)
    asm volatile(".rept 16\n\t"
                 "add %[a0], %[a0], #1\n\tadd %[a1], %[a1], #1\n\t"
                 "add %[a2], %[a2], #1\n\tadd %[a3], %[a3], #1\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3));
#else
    for (int index = 0; index < 16; ++index) { ++a0; ++a1; ++a2; ++a3; }
#endif
  }
  global_sink = global_sink ^ a0 ^ a1 ^ a2 ^ a3;
}

void add_eight_chains(std::size_t iterations) {
  std::uint64_t a0 = 1, a1 = 3, a2 = 5, a3 = 7;
  std::uint64_t a4 = 11, a5 = 13, a6 = 17, a7 = 19;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 8\n\t"
                 "addq $1, %[a0]\n\taddq $1, %[a1]\n\t"
                 "addq $1, %[a2]\n\taddq $1, %[a3]\n\t"
                 "addq $1, %[a4]\n\taddq $1, %[a5]\n\t"
                 "addq $1, %[a6]\n\taddq $1, %[a7]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                   [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#elif defined(__aarch64__)
    asm volatile(".rept 8\n\t"
                 "add %[a0], %[a0], #1\n\tadd %[a1], %[a1], #1\n\t"
                 "add %[a2], %[a2], #1\n\tadd %[a3], %[a3], #1\n\t"
                 "add %[a4], %[a4], #1\n\tadd %[a5], %[a5], #1\n\t"
                 "add %[a6], %[a6], #1\n\tadd %[a7], %[a7], #1\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                   [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#else
    for (int index = 0; index < 8; ++index) {
      ++a0; ++a1; ++a2; ++a3; ++a4; ++a5; ++a6; ++a7;
    }
#endif
  }
  global_sink = global_sink ^ a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

void multiply_one_chain(std::size_t iterations) {
  std::uint64_t value = 3;
  const std::uint64_t multiplier = 0x9e3779b1U;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 32\n\timulq %[multiplier], %[value]\n\t.endr"
                 : [value] "+r"(value) : [multiplier] "r"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 32\n\tmul %[value], %[value], %[multiplier]\n\t.endr"
                 : [value] "+r"(value) : [multiplier] "r"(multiplier));
#else
    for (int index = 0; index < 32; ++index) value *= multiplier;
#endif
  }
  global_sink = global_sink ^ value;
}

void multiply_four_chains(std::size_t iterations) {
  std::uint64_t a0 = 3, a1 = 5, a2 = 7, a3 = 11;
  const std::uint64_t multiplier = 0x9e3779b1U;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 8\n\t"
                 "imulq %[m], %[a0]\n\timulq %[m], %[a1]\n\t"
                 "imulq %[m], %[a2]\n\timulq %[m], %[a3]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3)
                 : [m] "r"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 8\n\t"
                 "mul %[a0], %[a0], %[m]\n\tmul %[a1], %[a1], %[m]\n\t"
                 "mul %[a2], %[a2], %[m]\n\tmul %[a3], %[a3], %[m]\n\t.endr"
                 : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3)
                 : [m] "r"(multiplier));
#else
    for (int index = 0; index < 8; ++index) {
      a0 *= multiplier; a1 *= multiplier; a2 *= multiplier; a3 *= multiplier;
    }
#endif
  }
  global_sink = global_sink ^ a0 ^ a1 ^ a2 ^ a3;
}

void fp_add_one_chain(std::size_t iterations) {
  double value = 1.0;
  const double increment = 0.0000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 32\n\taddsd %[increment], %[value]\n\t.endr"
                 : [value] "+x"(value) : [increment] "x"(increment));
#elif defined(__aarch64__)
    asm volatile(".rept 32\n\tfadd %d[value], %d[value], %d[increment]\n\t.endr"
                 : [value] "+w"(value) : [increment] "w"(increment));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(value);
}

void fp_add_four_chains(std::size_t iterations) {
  double a0 = 1.0, a1 = 2.0, a2 = 3.0, a3 = 4.0;
  const double increment = 0.0000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 8\n\t"
                 "addsd %[increment], %[a0]\n\taddsd %[increment], %[a1]\n\t"
                 "addsd %[increment], %[a2]\n\taddsd %[increment], %[a3]\n\t.endr"
                 : [a0] "+x"(a0), [a1] "+x"(a1), [a2] "+x"(a2), [a3] "+x"(a3)
                 : [increment] "x"(increment));
#elif defined(__aarch64__)
    asm volatile(".rept 8\n\t"
                 "fadd %d[a0], %d[a0], %d[increment]\n\t"
                 "fadd %d[a1], %d[a1], %d[increment]\n\t"
                 "fadd %d[a2], %d[a2], %d[increment]\n\t"
                 "fadd %d[a3], %d[a3], %d[increment]\n\t.endr"
                 : [a0] "+w"(a0), [a1] "+w"(a1), [a2] "+w"(a2), [a3] "+w"(a3)
                 : [increment] "w"(increment));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(a0 + a1 + a2 + a3);
}

void fp_multiply_one_chain(std::size_t iterations) {
  double value = 1.0001;
  const double multiplier = 1.0000000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 16\n\tmulsd %[multiplier], %[value]\n\t.endr"
                 : [value] "+x"(value) : [multiplier] "x"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 16\n\tfmul %d[value], %d[value], %d[multiplier]\n\t.endr"
                 : [value] "+w"(value) : [multiplier] "w"(multiplier));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(value);
}

void fp_multiply_four_chains(std::size_t iterations) {
  double a0 = 1.0001, a1 = 1.0002, a2 = 1.0003, a3 = 1.0004;
  const double multiplier = 1.0000000001;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
#if defined(__x86_64__)
    asm volatile(".rept 4\n\t"
                 "mulsd %[multiplier], %[a0]\n\tmulsd %[multiplier], %[a1]\n\t"
                 "mulsd %[multiplier], %[a2]\n\tmulsd %[multiplier], %[a3]\n\t.endr"
                 : [a0] "+x"(a0), [a1] "+x"(a1), [a2] "+x"(a2), [a3] "+x"(a3)
                 : [multiplier] "x"(multiplier));
#elif defined(__aarch64__)
    asm volatile(".rept 4\n\t"
                 "fmul %d[a0], %d[a0], %d[multiplier]\n\t"
                 "fmul %d[a1], %d[a1], %d[multiplier]\n\t"
                 "fmul %d[a2], %d[a2], %d[multiplier]\n\t"
                 "fmul %d[a3], %d[a3], %d[multiplier]\n\t.endr"
                 : [a0] "+w"(a0), [a1] "+w"(a1), [a2] "+w"(a2), [a3] "+w"(a3)
                 : [multiplier] "w"(multiplier));
#endif
  }
  global_sink = global_sink ^ static_cast<std::uint64_t>(a0 + a1 + a2 + a3);
}

void record_kernel(std::vector<Observation> &observations, const std::string &name,
                   std::size_t operations, const KernelMeasurement &measurement,
                   std::vector<std::string> &warnings,
                   const Labels &extra_labels = {}) {
  const double ops = static_cast<double>(operations);
  Labels labels = extra_labels;
  labels["kernel"] = name;
  add_observation(observations, "pipeline", "operation_throughput", ops / measurement.seconds / 1e9,
                  "Gop/s", "medium", "architecture-specific scalar assembly microkernel", labels);
  add_observation(observations, "pipeline", "counter_tick_efficiency",
                  ops / static_cast<double>(measurement.counter_ticks), "ops/counter-tick", "low",
                  "operations divided by invariant/platform counter ticks", labels);
  if (measurement.perf.available && measurement.perf.cycles > 0.0) {
    add_observation(observations, "pipeline", "effective_core_frequency",
                    measurement.perf.cycles / measurement.seconds / 1e9, "GHz", "medium",
                    "perf core cycles divided by wall time during kernel", labels);
    add_observation(observations, "pipeline", "operations_per_cycle",
                    ops / measurement.perf.cycles, "ops/cycle", "high",
                    "known operations divided by perf core cycles", labels);
    add_observation(observations, "pipeline", "ipc",
                    measurement.perf.instructions / measurement.perf.cycles,
                    "instructions/cycle", "high", "perf retired instructions / core cycles", labels);
    add_observation(observations, "pipeline", "cycles_per_operation",
                    measurement.perf.cycles / ops, "cycles/op", "high",
                    "perf core cycles divided by known operations", labels);
    if (measurement.perf.cache_counters_available && measurement.perf.cache_references > 0.0) {
      add_observation(observations, "pipeline", "cache_miss_rate",
                      100.0 * measurement.perf.cache_misses / measurement.perf.cache_references,
                      "%", "medium", "generic perf cache misses/references", labels);
    }
  } else if (!measurement.perf.error.empty()) {
    add_warning_once(warnings, "核心流水线 PMU 测量失败：" + measurement.perf.error +
                               "；IPC、每周期吞吐与周期延迟已标记为不可用");
  }
}

void benchmark_pipeline(const Options &options, std::vector<Observation> &observations,
                        std::vector<std::string> &warnings, int cpu) {
  pin_to_cpu(cpu);
  const std::size_t iterations = options.profile == "smoke" ? 2000
      : options.profile == "quick" ? 100000
      : options.profile == "deep" ? 2000000
                                  : 500000;
  auto nops = measure_kernel([&] { nop_kernel(iterations); });
  record_kernel(observations, "nop_frontend", iterations * 128, nops, warnings,
                {{"chains", "n/a"}, {"bound", "frontend"}});
  auto add1 = measure_kernel([&] { add_one_chain(iterations); });
  record_kernel(observations, "integer_add_dependency", iterations * 64, add1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}});
  auto add4 = measure_kernel([&] { add_four_chains(iterations); });
  record_kernel(observations, "integer_add_parallel4", iterations * 64, add4, warnings,
                {{"chains", "4"}, {"bound", "backend"}});
  auto add8 = measure_kernel([&] { add_eight_chains(iterations); });
  record_kernel(observations, "integer_add_parallel8", iterations * 64, add8, warnings,
                {{"chains", "8"}, {"bound", "backend"}});
  auto mul1 = measure_kernel([&] { multiply_one_chain(iterations); });
  record_kernel(observations, "integer_mul_dependency", iterations * 32, mul1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}});
  auto mul4 = measure_kernel([&] { multiply_four_chains(iterations); });
  record_kernel(observations, "integer_mul_parallel4", iterations * 32, mul4, warnings,
                {{"chains", "4"}, {"bound", "backend"}});
  auto fp_add1 = measure_kernel([&] { fp_add_one_chain(iterations); });
  record_kernel(observations, "fp64_add_dependency", iterations * 32, fp_add1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}, {"class", "floating-point"}});
  auto fp_add4 = measure_kernel([&] { fp_add_four_chains(iterations); });
  record_kernel(observations, "fp64_add_parallel4", iterations * 32, fp_add4, warnings,
                {{"chains", "4"}, {"bound", "backend"}, {"class", "floating-point"}});
  auto fp_mul1 = measure_kernel([&] { fp_multiply_one_chain(iterations); });
  record_kernel(observations, "fp64_mul_dependency", iterations * 16, fp_mul1, warnings,
                {{"chains", "1"}, {"bound", "dependency"}, {"class", "floating-point"}});
  auto fp_mul4 = measure_kernel([&] { fp_multiply_four_chains(iterations); });
  record_kernel(observations, "fp64_mul_parallel4", iterations * 16, fp_mul4, warnings,
                {{"chains", "4"}, {"bound", "backend"}, {"class", "floating-point"}});
}

void run(ProbeContext &context) {
  benchmark_pipeline(context.options, context.observations, context.warnings,
                       context.primary_cpu);
}

}  // namespace

ProbeDefinition pipeline_probe() {
  return {"pipeline", "core pipeline and IPC", "核心前后端、IPC 与标量执行吞吐", run};
}

}  // namespace sdc

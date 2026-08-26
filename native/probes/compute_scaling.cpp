#include "../probe.hpp"
#include "../support/pointer_chase.hpp"
#include "../support/stream.hpp"

namespace sdc {
namespace {

std::uint64_t compute_add_batch(std::uint64_t seed) {
  std::uint64_t a0 = seed + 1, a1 = seed + 3, a2 = seed + 5, a3 = seed + 7;
  std::uint64_t a4 = seed + 11, a5 = seed + 13, a6 = seed + 17, a7 = seed + 19;
#if defined(__x86_64__)
  asm volatile(".rept 32\n\t"
               "addq $1, %[a0]\n\taddq $1, %[a1]\n\taddq $1, %[a2]\n\taddq $1, %[a3]\n\t"
               "addq $1, %[a4]\n\taddq $1, %[a5]\n\taddq $1, %[a6]\n\taddq $1, %[a7]\n\t.endr"
               : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                 [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#elif defined(__aarch64__)
  asm volatile(".rept 32\n\t"
               "add %[a0], %[a0], #1\n\tadd %[a1], %[a1], #1\n\t"
               "add %[a2], %[a2], #1\n\tadd %[a3], %[a3], #1\n\t"
               "add %[a4], %[a4], #1\n\tadd %[a5], %[a5], #1\n\t"
               "add %[a6], %[a6], #1\n\tadd %[a7], %[a7], #1\n\t.endr"
               : [a0] "+r"(a0), [a1] "+r"(a1), [a2] "+r"(a2), [a3] "+r"(a3),
                 [a4] "+r"(a4), [a5] "+r"(a5), [a6] "+r"(a6), [a7] "+r"(a7));
#endif
  return a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7;
}

void measure_compute_scaling_point(const Options &options, const std::vector<int> &selected,
                                   const std::string &scope,
                                   std::vector<Observation> &observations) {
  struct Result {
    std::uint64_t operations = 0;
    std::uint64_t sink = 0;
    double seconds = 0.0;
    PerfCounts perf;
  };
  std::vector<Result> results(selected.size());
  std::atomic<std::size_t> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < selected.size(); ++index) {
    workers.emplace_back([&, index] {
      pin_to_cpu(selected[index]);
      PerfGroup perf;
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) cpu_relax();
      const auto begin = Clock::now();
      perf.start();
      std::uint64_t sink = index + 1;
      std::uint64_t operations = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        sink ^= compute_add_batch(sink);
        operations += 256;
      }
      results[index].perf = perf.stop();
      results[index].seconds = seconds_between(begin, Clock::now());
      results[index].operations = operations;
      results[index].sink = sink;
    });
  }
  while (ready.load(std::memory_order_acquire) != selected.size()) std::this_thread::yield();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(
      std::max(30, options.profile == "quick" ? options.duration_ms / 2 : options.duration_ms)));
  stop.store(true, std::memory_order_release);
  for (auto &worker : workers) worker.join();

  std::uint64_t total_operations = 0;
  double elapsed = 0.0;
  std::vector<double> frequencies;
  std::ostringstream cpu_list;
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (index) cpu_list << ',';
    cpu_list << selected[index];
    total_operations += results[index].operations;
    elapsed = std::max(elapsed, results[index].seconds);
    global_sink = global_sink ^ results[index].sink;
    if (results[index].perf.available && results[index].seconds > 0.0) {
      frequencies.push_back(results[index].perf.cycles / results[index].seconds / 1e9);
    }
  }
  const Labels labels{{"threads", std::to_string(selected.size())},
                      {"scope", scope}, {"cpus", cpu_list.str()}};
  add_observation(observations, "compute_scaling", "integer_add_throughput",
                  static_cast<double>(total_operations) / std::max(elapsed, 1e-12) / 1e9,
                  "Gop/s", "medium", "parallel pinned independent integer-add chains", labels);
  if (!frequencies.empty()) {
    add_observation(observations, "compute_scaling", "effective_core_frequency",
                    median_double(std::move(frequencies)), "GHz", "medium",
                    "median perf core cycles per wall second across active workers", labels);
  }
}

void benchmark_compute_scaling(const Options &options, const std::vector<CpuInfo> &cpus,
                               std::vector<Observation> &observations) {
  if (options.profile == "smoke") return;
  const auto physical = numa_balanced_physical_cpus(cpus);
  const std::size_t maximum = options.profile == "quick"
      ? std::min<std::size_t>(physical.size(), 4) : physical.size();
  for (const std::size_t count : thread_counts(maximum)) {
    std::vector<int> selected;
    for (std::size_t index = 0; index < count; ++index) selected.push_back(physical[index].cpu);
    measure_compute_scaling_point(options, selected, "physical-cores", observations);
  }
  for (std::size_t left = 0; left < cpus.size(); ++left) {
    bool found = false;
    for (std::size_t right = left + 1; right < cpus.size(); ++right) {
      if (cpu_relation(cpus[left], cpus[right]) == "smt-sibling") {
        measure_compute_scaling_point(options, {cpus[left].cpu, cpus[right].cpu},
                                      "smt-siblings", observations);
        found = true;
        break;
      }
    }
    if (found) break;
  }
}

void run(ProbeContext &context) {
  benchmark_compute_scaling(context.options, context.cpus, context.observations);
}

}  // namespace

ProbeDefinition compute_scaling_probe() {
  return {"compute-scaling", "multi-core and SMT compute scaling", "物理核心与 SMT 计算扩展", run};
}

}  // namespace sdc

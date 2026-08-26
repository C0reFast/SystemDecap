#include "../probe.hpp"

namespace sdc {
namespace {

double false_sharing_rate(int cpu_a, int cpu_b, std::size_t separation, int duration_ms) {
  MappedBuffer buffer(4096);
  auto *base = buffer.bytes();
  auto *first = new (base) std::atomic<std::uint64_t>(0);
  auto *second = new (base + separation) std::atomic<std::uint64_t>(0);
  std::atomic<int> ready{0};
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::uint64_t first_count = 0;
  std::uint64_t second_count = 0;
  auto worker = [&](int cpu, std::atomic<std::uint64_t> *value, std::uint64_t &count) {
    pin_to_cpu(cpu);
    ready.fetch_add(1, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) cpu_relax();
    while (!stop.load(std::memory_order_relaxed)) {
      for (int index = 0; index < 64; ++index) {
        value->fetch_add(1, std::memory_order_relaxed);
      }
      count += 64;
    }
  };
  std::thread one(worker, cpu_a, first, std::ref(first_count));
  std::thread two(worker, cpu_b, second, std::ref(second_count));
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  const auto begin = Clock::now();
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
  stop.store(true, std::memory_order_release);
  one.join();
  two.join();
  const double elapsed = seconds_between(begin, Clock::now());
  std::destroy_at(first);
  std::destroy_at(second);
  return static_cast<double>(first_count + second_count) / elapsed / 1e6;
}

void benchmark_false_sharing(const Options &options, const std::vector<CpuInfo> &cpus,
                             std::vector<Observation> &observations) {
  const auto physical = physical_cpus(cpus);
  if (physical.size() < 2 || options.profile == "smoke") return;
  const int cpu_a = physical[0].cpu;
  const int cpu_b = physical[1].cpu;
  for (const std::size_t separation : {8U, 16U, 32U, 64U, 128U, 256U}) {
    const double rate = false_sharing_rate(cpu_a, cpu_b, separation,
                                           std::max(20, options.duration_ms / 2));
    add_observation(observations, "coherence", "atomic_update_rate", rate, "Mop/s",
                    "medium", "two independent atomics at varying byte separation",
                    {{"cpu_a", std::to_string(cpu_a)},
                     {"cpu_b", std::to_string(cpu_b)},
                     {"separation_bytes", std::to_string(separation)}});
  }
}

void run(ProbeContext &context) {
  benchmark_false_sharing(context.options, context.cpus, context.observations);
}

}  // namespace

ProbeDefinition false_sharing_probe() {
  return {"false-sharing", "false sharing / coherence line", "伪共享敏感度与一致性行边界", run};
}

}  // namespace sdc

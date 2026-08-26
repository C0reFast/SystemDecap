#include "../probe.hpp"

namespace sdc {
namespace {

void benchmark_timers(std::vector<Observation> &observations, int cpu) {
  pin_to_cpu(cpu);
  constexpr std::size_t samples = 200000;
  auto begin = Clock::now();
  for (std::size_t index = 0; index < samples; ++index) {
    asm volatile("" ::: "memory");
    const auto stamp = Clock::now();
    asm volatile("" : : "g"(&stamp) : "memory");
  }
  auto end = Clock::now();
  add_observation(observations, "timer", "steady_clock_call",
                  seconds_between(begin, end) * 1e9 / static_cast<double>(samples),
                  "ns/call", "high", "back-to-back steady_clock calls", {{"cpu", std::to_string(cpu)}});

  std::uint64_t minimum = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t total = 0;
  for (std::size_t index = 0; index < samples; ++index) {
    const auto first = cycle_counter();
    const auto second = cycle_counter();
    const auto delta = second - first;
    minimum = std::min(minimum, delta);
    total += delta;
  }
  add_observation(observations, "timer", "cycle_counter_min", static_cast<double>(minimum),
                  "ticks/call-pair", "high", "serialized back-to-back counter reads",
                  {{"cpu", std::to_string(cpu)}});
  add_observation(observations, "timer", "cycle_counter_mean",
                  static_cast<double>(total) / static_cast<double>(samples),
                  "ticks/call-pair", "high", "serialized back-to-back counter reads",
                  {{"cpu", std::to_string(cpu)}});

  const auto wall_begin = Clock::now();
  const auto cycle_begin = cycle_counter();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  const auto cycle_end = cycle_counter();
  const auto wall_end = Clock::now();
  const double hz = static_cast<double>(cycle_end - cycle_begin) /
                    seconds_between(wall_begin, wall_end);
  add_observation(observations, "timer", "counter_frequency", hz / 1e9, "GHz",
                  architecture_name() == "x86_64" ? "high" : "medium",
                  "counter delta over monotonic wall interval", {{"cpu", std::to_string(cpu)}});
}

void run(ProbeContext &context) {
  benchmark_timers(context.observations, context.primary_cpu);
}

}  // namespace

ProbeDefinition timer_probe() {
  return {"timer", "timer calibration", "计时器调用开销与平台计数器校准", run};
}

}  // namespace sdc

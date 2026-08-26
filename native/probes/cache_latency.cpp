#include "../probe.hpp"
#include "../support/pointer_chase.hpp"

namespace sdc {
namespace {

void benchmark_cache_latency(const Options &options, std::vector<Observation> &observations,
                             std::vector<std::string> &warnings, int cpu) {
  pin_to_cpu(cpu);
  const std::size_t llc = last_level_cache_bytes(cpu);
  const std::size_t base_last = options.profile == "smoke" ? 64U * 1024U
      : options.profile == "quick" ? 32U * 1024U * 1024U
      : options.profile == "deep" ? 512U * 1024U * 1024U
                                  : 128U * 1024U * 1024U;
  const std::size_t multiplier = options.profile == "deep" ? 4 : 2;
  const std::size_t cap = options.profile == "quick" ? 512ULL * 1024ULL * 1024ULL
                                                     : 2ULL * 1024ULL * 1024ULL * 1024ULL;
  const std::size_t last = options.profile == "smoke"
      ? base_last : std::min(cap, std::max(base_last, llc * multiplier));
  std::mt19937 random(options.seed);
  try {
    for (const auto bytes : power_sizes(4U * 1024U, last)) {
      MappedBuffer buffer(bytes);
      build_random_chain(buffer.bytes(), bytes, 64, random);
      const std::size_t points = bytes / 64;
      const std::size_t iterations = std::clamp<std::size_t>(points * 12, 100000, 5000000);
      (void)chase_chain(buffer.bytes(), std::min<std::size_t>(iterations, points * 2));
      const int samples = options.profile == "smoke" ? 1 : options.profile == "quick" ? 3 : 5;
      std::vector<double> latencies;
      for (int sample = 0; sample < samples; ++sample)
        latencies.push_back(chase_chain(buffer.bytes(), iterations));
      const double latency = median_double(std::move(latencies));
      add_observation(observations, "cache_latency", "random_load_latency", latency,
                      "ns/access", "high", "random dependent pointer chase",
                      {{"cpu", std::to_string(cpu)},
                       {"working_set_bytes", std::to_string(bytes)},
                       {"stride_bytes", "64"}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("缓存延迟探针提前停止：") + error.what());
  }
}

void run(ProbeContext &context) {
  benchmark_cache_latency(context.options, context.observations, context.warnings,
                          context.primary_cpu);
}

}  // namespace

ProbeDefinition cache_latency_probe() {
  return {"cache-latency", "cache hierarchy latency sweep", "缓存层级随机依赖加载延迟", run};
}

}  // namespace sdc

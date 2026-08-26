#include "../probe.hpp"

namespace sdc {
namespace {

void benchmark_stride_prefetch(const Options &options, int cpu,
                               std::vector<Observation> &observations,
                               std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  const std::size_t base_bytes = (options.profile == "quick" ? 64U : 256U) * 1024U * 1024U;
  const std::size_t cap = options.profile == "quick" ? 256ULL * 1024ULL * 1024ULL
                                                     : 1024ULL * 1024ULL * 1024ULL;
  const std::size_t bytes = std::min(cap, std::max(base_bytes, last_level_cache_bytes(cpu) * 2));
  try {
    MappedBuffer buffer(bytes);
    pin_to_cpu(cpu);
    auto *values = buffer.as<std::uint64_t>();
    const std::size_t elements = bytes / sizeof(std::uint64_t);
    for (std::size_t index = 0; index < elements; ++index) values[index] = index + 1;
    for (const std::size_t stride_bytes : {8U, 16U, 32U, 64U, 128U, 256U, 512U,
                                           1024U, 2048U, 4096U}) {
      const std::size_t stride = stride_bytes / sizeof(std::uint64_t);
      std::uint64_t sum = 0;
      std::uint64_t accesses = 0;
      const auto begin = Clock::now();
      double elapsed = 0.0;
      do {
        for (std::size_t index = 0; index < elements; index += stride) sum += values[index];
        accesses += (elements + stride - 1) / stride;
        elapsed = seconds_between(begin, Clock::now());
      } while (elapsed * 1000.0 < std::max(20, options.duration_ms / 2));
      global_sink = global_sink ^ sum;
      add_observation(observations, "memory_access", "stride_access_rate",
                      static_cast<double>(accesses) / elapsed / 1e9, "Gaccess/s", "medium",
                      "one-core sequential fixed-stride load sweep",
                      {{"cpu", std::to_string(cpu)},
                       {"stride_bytes", std::to_string(stride_bytes)},
                       {"working_set_bytes", std::to_string(bytes)}});
      add_observation(observations, "memory_access", "stride_payload_bandwidth",
                      static_cast<double>(accesses * sizeof(std::uint64_t)) / elapsed / 1e9,
                      "GB/s", "medium", "requested 8-byte payload only; cache-line traffic excluded",
                      {{"cpu", std::to_string(cpu)},
                       {"stride_bytes", std::to_string(stride_bytes)},
                       {"working_set_bytes", std::to_string(bytes)}});
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("步长/预取扫描提前停止：") + error.what());
  }
}

void run(ProbeContext &context) {
  benchmark_stride_prefetch(context.options, context.primary_cpu,
                              context.observations, context.warnings);
}

}  // namespace

ProbeDefinition stride_prefetch_probe() {
  return {"stride-prefetch", "stride and prefetch sensitivity", "访问步长与硬件预取敏感度", run};
}

}  // namespace sdc

#include "../probe.hpp"
#include "../support/stream.hpp"

namespace sdc {
namespace {

void benchmark_cache_bandwidth(const Options &options, const std::vector<CpuInfo> &cpus,
                               std::vector<Observation> &observations,
                               std::vector<std::string> &warnings) {
  if (options.profile == "smoke") return;
  const int cpu = physical_cpus(cpus).front().cpu;
  const std::size_t base_last = options.profile == "quick" ? 64U * 1024U * 1024U
      : options.profile == "deep" ? 512U * 1024U * 1024U
                                  : 256U * 1024U * 1024U;
  const std::size_t cap = options.profile == "quick" ? 256ULL * 1024ULL * 1024ULL
                                                     : 1024ULL * 1024ULL * 1024ULL;
  const std::size_t last = std::min(cap, std::max(base_last, last_level_cache_bytes(cpu) * 2));
  try {
    for (const auto bytes : power_sizes(32U * 1024U, last)) {
      for (const auto operation : {StreamOperation::Read, StreamOperation::Copy}) {
        const auto result = run_stream(operation, {cpu}, bytes,
                                       std::max(20, options.duration_ms / 2));
        add_observation(observations, "cache_bandwidth", "working_set_bandwidth",
                        result.gigabytes_per_second, "GB/s", "medium",
                        "one-core repeated streaming kernel by working-set size",
                        {{"cpu", std::to_string(cpu)},
                         {"operation", stream_name(operation)},
                         {"working_set_bytes", std::to_string(bytes)}});
      }
    }
  } catch (const std::exception &error) {
    warnings.push_back(std::string("缓存带宽扫描提前停止：") + error.what());
  }
}

void run(ProbeContext &context) {
  benchmark_cache_bandwidth(context.options, context.cpus, context.observations,
                              context.warnings);
}

}  // namespace

ProbeDefinition cache_bandwidth_probe() {
  return {"cache-bandwidth", "cache bandwidth by working set", "单核缓存带宽工作集扫描", run};
}

}  // namespace sdc
